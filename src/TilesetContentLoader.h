// SPDX-License-Identifier: Unlicense
//
// TilesetContentLoader: gets a tile payload and turns it into a decodable model, off the
// main thread.
//
// Why this exists
// ---------------
// The earlier scheduler read AND decoded payloads inline, inside NOTIFICATION_PROCESS.
// Decoding a b3dm means running Draco and transcoding KTX2 - milliseconds per tile - so
// eight of those per frame is a visible hitch, and a cold start on a large dataset stalls
// for seconds. The work is almost entirely independent between tiles, so the fix is to move
// it off the main thread rather than to do less of it.
//
// The split is three-stage, and only the last stage touches Godot:
//
//   1. **fetch**  - get the bytes. Local files are read on WorkerThreadPool; remote URLs go
//                   through Godot's HTTPRequest so the engine's own client handles proxies,
//                   TLS, redirects and gzip rather than anything hand-rolled here.
//   2. **decode** - unwrap the container and run Draco + KTX2. Pure kernel code, no engine
//                   types, so it is safe on a worker thread. This is the expensive half.
//   3. **assemble** - create ArrayMesh / StandardMaterial3D / Node3D and attach. MUST be on
//                   the main thread, so this class stops at stage 2 and hands the decoded
//                   model back.
//
// The main thread therefore only ever does stage 3, which is bounded and cheap; frame time
// stays flat as the dataset grows.

#ifndef TILESET_CONTENT_LOADER_H
#define TILESET_CONTENT_LOADER_H

#include "ContentFactory.h"

#include "core/tiles/Tile.h"

#include "godot_cpp/classes/node.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/string.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace tiles3d
{
    /// State of a load request. A request only ever moves forward.
    enum class LoadState
    {
        Fetching, ///< bytes are on their way (worker thread or HTTPRequest)
        Decoding, ///< bytes are here, Draco/KTX2 running on a worker thread
        Ready,    ///< `decoded` is usable
        Failed,   ///< `error` explains why
    };

    /// A completed or failed load, handed to the owner for adoption.
    struct CompletedLoad
    {
        core::Tile *tile = nullptr;
        godot::String source;
        std::uint64_t generation = 0;

        LoadState state = LoadState::Failed;

        /// Payload statistics, for diagnostics.
        std::size_t byteCount = 0;

        /// Valid when `state == Ready`.
        ///
        /// Deliberately already *prepared* - meshes, materials and textures built on the
        /// worker thread - rather than raw bytes or a decoded model. The main thread's
        /// remaining work is then only node instantiation, which is the split cesium-native
        /// uses between prepareInLoadThread and prepareInMainThread: do everything that does
        /// not need the scene tree on the load thread, and leave the main thread the minimum.
        std::unique_ptr<PreparedContent> prepared;

        /// Why it failed, when it did.
        godot::String error;
    };

    /// Priority key for the load queue. Lower sorts first.
    /// Mirrors `compareLoadPriority` in the reference scheduler, which mirrors Cesium's
    /// `sortTilesByPriority`: whatever the camera is actually looking at arrives first, since
    /// that is the only part the user can see mid-flight.
    struct LoadPriority
    {
        /// Screen-space factor; smaller is nearer the centre of the view.
        double foveated = 0.0;

        /// Distance from the camera to the tile surface, in metres.
        double distance = 0.0;

        /// Depth in the tree, used only to break ties.
        int depth = 0;

        /// When true, deeper tiles win ties (the reference's `preferLeaves`).
        bool preferLeaves = false;
    };

    /// True when `a` should be loaded before `b`.
    bool isHigherLoadPriority( const LoadPriority &a, const LoadPriority &b );

    class TilesetContentLoader : public godot::Node
    {
        GDCLASS( TilesetContentLoader, godot::Node )

    public:
        /// One request bookkeeping record. Public only so the file-local worker functions can
        /// name it; treat it as opaque.
        ///
        /// Defined in the header rather than the .cpp so that `std::unique_ptr<Request>` can
        /// be destroyed here: `default_delete` needs the complete type, and hiding it in the
        /// .cpp forces the destructor (and therefore the whole class) out of line, which is
        /// what broke godot-cpp's `memnew` initialisation.
        struct Request
        {
            core::Tile *tile = nullptr;
            godot::String source;
            std::uint64_t generation = 0;

            /// Set by the worker before it stores its result, so a reader that sees
            /// `finished` is guaranteed to see the payload too (release/acquire pairing).
            std::atomic<bool> finished{ false };
            std::atomic<bool> cancelled{ false };

            /// Worker-pool handle while the job is running; -1 once reaped.
            int64_t taskId = -1;

            /// Set by the fetch stage.
            godot::PackedByteArray bytes;
            godot::String fetchError;

            /// The declaration-stated up axis, carried from the fetch stage to the prepare
            /// stage.
            core::ModelUpAxis upAxis = core::ModelUpAxis::Y;

            /// Set by the prepare stage: meshes, materials and textures, built off-thread.
            std::unique_ptr<PreparedContent> prepared;
            godot::String prepareError;
        };

        TilesetContentLoader();
        ~TilesetContentLoader() override;

        void set_max_concurrent( int p_value );
        int get_max_concurrent() const;

        /// True when the given number of jobs are already in flight.
        bool is_saturated() const;

        /// Number of requests currently in flight.
        std::size_t get_active_count() const;

        /// Total payload bytes transferred by completed fetches.
        std::uint64_t get_bytes_transferred() const;

        /// Begins fetching, decoding and preparing `source`. Returns false when the loader is
        /// saturated or the tile already has a request, in which case the caller should retry
        /// later.
        ///
        /// `upAxis` is the tileset's declared content up axis, needed during preparation.
        bool request( core::Tile *tile, const godot::String &source, core::ModelUpAxis upAxis );

        /// Cancels a tile's request, if any. Its result is discarded if it later arrives.
        void cancel( core::Tile *tile );

        /// Drops everything whose generation is older than `generation` (used on reload).
        void cancel_before_generation( std::uint64_t generation );

        /// Bumps the generation, invalidating every in-flight request.
        void bump_generation();

        /// The generation in-flight requests are currently being stamped with.
        std::uint64_t get_generation() const;

        /// Jobs that finished since the last call. The owner adopts each one: `decoded` is
        /// moved into the tile, then the record is dropped by scope exit.
        std::vector<CompletedLoad> collect_completed();

        /// True while `tile` has a request in flight.
        bool has_request( core::Tile *tile ) const;

        /// Fetches a remote payload into `request->bytes` with a private HTTPClient that is
        /// polled to completion, then returns true. On failure it fills `request->fetchError`
        /// and returns false.
        ///
        /// Static and public rather than a private member because the file-local worker that
        /// `WorkerThreadPool` runs cannot see a private member: the pool only accepts a plain
        /// function pointer, so no closure can be used to smuggle `this` in. It touches no
        /// loader state - only the request - so a free-standing call is honest about that
        /// rather than widening access for appearances.
        static bool fetch_http( Request *p_request );

    private:
        /// Number of requests in flight.
        std::size_t active_count() const;

    protected:
        static void _bind_methods();

    private:
        /// Finds a request by tile. Returns null when there is none.
        Request *find_request( core::Tile *tile );

        /// Unlinks a finished request and publishes it as a completed load.
        void publish( Request *request, LoadState state, const godot::String &error );

        /// Runs everything still in flight to completion, then releases the records. Used at
        /// shutdown: a worker holds a pointer into `requests_`, so this object must not be
        /// destroyed while one is running.
        void drain();

        int max_concurrent_ = 20;
        std::uint64_t generation_ = 0;
        std::uint64_t bytes_transferred_ = 0;

        std::vector<std::unique_ptr<Request>> requests_;
        std::vector<CompletedLoad> completed_;
    };

} // namespace tiles3d

#endif // TILESET_CONTENT_LOADER_H
