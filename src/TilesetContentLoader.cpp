// SPDX-License-Identifier: Unlicense

#include "TilesetContentLoader.h"

#include "core/io/Url.h"

#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/worker_thread_pool.hpp"
#include "godot_cpp/core/memory.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include "godot_cpp/variant/utility_functions.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace tiles3d
{
    using godot::FileAccess;
    using godot::Ref;
    bool isHigherLoadPriority( const LoadPriority &a, const LoadPriority &b )
    {
        // Ported from compareLoadPriority in the reference scheduler, which ports Cesium's
        // sortTilesByPriority. The order is deliberate: what is visible now must arrive before
        // what is merely nearby, because a load nobody can see is wasted latency.
        if ( a.foveated != b.foveated )
        {
            return a.foveated < b.foveated;
        }

        if ( a.distance != b.distance )
        {
            return a.distance < b.distance;
        }

        return a.preferLeaves ? ( a.depth > b.depth ) : ( a.depth < b.depth );
    }

    namespace
    {
        /// Decode + prepare a payload that is already in `request->bytes`.
        ///
        /// This is the expensive half and the reason the loader exists: container unwrapping,
        /// Draco decompression, KTX2 transcoding, and building the ArrayMesh / material /
        /// texture resources. All of it is safe off the main thread because none of those
        /// resources are in the scene tree yet.
        void prepare_request( TilesetContentLoader::Request *request )
        {
            DecodedTileContent decoded = decodeTileContent( request->bytes );
            if ( !decoded.error.empty() )
            {
                request->prepareError = godot::String( decoded.error.c_str() );
                return;
            }

            auto prepared =
                std::make_unique<PreparedContent>( prepareContentResources( decoded, request->upAxis ) );

            if ( !prepared->error.empty() )
            {
                request->prepareError = godot::String( prepared->error.c_str() );
                return;
            }

            request->prepared = std::move( prepared );
        }

        /// Fetch + decode + prepare a local file. Runs on a worker thread.
        void run_local_request( void *userdata )
        {
            auto *request = static_cast<TilesetContentLoader::Request *>( userdata );

            if ( request->cancelled.load( std::memory_order_acquire ) )
            {
                request->finished.store( true, std::memory_order_release );
                return;
            }

            // A fresh READ handle per call: FileAccess is fine with concurrent independent
            // readers, and this is the pattern the engine's own loader threads use.
            const Ref<FileAccess> file =
                FileAccess::open( request->source, FileAccess::READ );
            if ( file.is_null() )
            {
                request->fetchError = godot::vformat( "cannot open '%s' (error %d)",
                                                      request->source,
                                                      static_cast<int>( FileAccess::get_open_error() ) );
                request->finished.store( true, std::memory_order_release );
                return;
            }

            request->bytes = file->get_buffer( static_cast<std::int64_t>( file->get_length() ) );
            file->close();

            if ( request->bytes.size() == 0 )
            {
                request->fetchError = godot::vformat( "'%s' is empty", request->source );
                request->finished.store( true, std::memory_order_release );
                return;
            }

            if ( request->cancelled.load( std::memory_order_acquire ) )
            {
                request->finished.store( true, std::memory_order_release );
                return;
            }

            prepare_request( request );

            request->finished.store( true, std::memory_order_release );
        }

        /// Fetch + decode + prepare a remote payload. Runs on a worker thread.
        ///
        /// Uses a private HTTPClient polled to completion rather than Godot's HTTPRequest
        /// node, which is what the pre-refactor accessor did and what Tileset3D already does
        /// for its control documents. HTTPRequest is signal-driven and its completion is
        /// dispatched by the engine's own emission pass; driving it from a scheduler that also
        /// has to cancel, reorder and budget the same transfers means fighting that pass for
        /// control, and every step of it (disconnecting mid-emission, cancel_request from
        /// _process) deadlocks the main thread. A client that is simply polled has none of
        /// those interactions: the work happens on this thread, and the main thread only ever
        /// sees the finished bytes.
        ///
        /// The userdata is the payload from `WorkerThreadPool::add_native_task`: a single
        /// `void *`, and the pool accepts nothing richer than a plain function pointer, so the
        /// request record is passed directly and the fetch helper is a static member.
        void run_remote_request( void *userdata )
        {
            auto *request = static_cast<TilesetContentLoader::Request *>( userdata );

            if ( request->cancelled.load( std::memory_order_acquire ) )
            {
                request->finished.store( true, std::memory_order_release );
                return;
            }

            if ( !TilesetContentLoader::fetch_http( request ) )
            {
                request->finished.store( true, std::memory_order_release );
                return;
            }

            if ( request->cancelled.load( std::memory_order_acquire ) )
            {
                request->finished.store( true, std::memory_order_release );
                return;
            }

            prepare_request( request );

            request->finished.store( true, std::memory_order_release );
        }
    } // namespace

    // Out of line rather than `= default`: the declaration site keeps the class layout in the
    // header while the body stays here, which is what the Godot binding macros expect.
    TilesetContentLoader::TilesetContentLoader() = default;

    TilesetContentLoader::~TilesetContentLoader()
    {
        drain();
    }

    void TilesetContentLoader::_bind_methods() {}

    void TilesetContentLoader::set_max_concurrent( int p_value )
    {
        max_concurrent_ = std::max( 1, p_value );
    }

    int TilesetContentLoader::get_max_concurrent() const
    {
        return max_concurrent_;
    }

    bool TilesetContentLoader::is_saturated() const
    {
        // HTTP requests count against the budget from the moment they start, so a slow
        // network cannot pile up unbounded work.
        return static_cast<int>( requests_.size() ) >= max_concurrent_;
    }

    std::size_t TilesetContentLoader::get_active_count() const
    {
        return requests_.size();
    }

    std::uint64_t TilesetContentLoader::get_bytes_transferred() const
    {
        return bytes_transferred_;
    }

    bool TilesetContentLoader::has_request( core::Tile *tile ) const
    {
        return std::any_of( requests_.begin(), requests_.end(),
                            [tile]( const std::unique_ptr<Request> &r )
                            { return r->tile == tile; } );
    }

    TilesetContentLoader::Request *TilesetContentLoader::find_request( core::Tile *tile )
    {
        for ( const std::unique_ptr<Request> &request : requests_ )
        {
            if ( request->tile == tile )
            {
                return request.get();
            }
        }
        return nullptr;
    }

    bool TilesetContentLoader::request( core::Tile *tile, const godot::String &source,
                                        core::ModelUpAxis upAxis )
    {
        if ( tile == nullptr || source.is_empty() || is_saturated() || has_request( tile ) )
        {
            return false;
        }

        auto owned = std::make_unique<Request>();
        Request *request = owned.get();
        request->tile = tile;
        request->source = source;
        request->generation = generation_;
        request->upAxis = upAxis;

        requests_.push_back( std::move( owned ) );

        // Both transports run on a worker thread. Local reads and HTTP polls are the same
        // shape from here: get the bytes, then decode and prepare them. Doing the HTTP poll
        // off the main thread is what makes the concurrency real rather than cooperative -
        // twenty transfers progress genuinely in parallel instead of being advanced one poll
        // at a time by the frame loop.
        godot::WorkerThreadPool *pool = godot::WorkerThreadPool::get_singleton();
        const bool remote = core::isRemoteUrl( source.utf8().get_data() );
        if ( pool == nullptr )
        {
            // No pool (very early startup, or a stripped build): do it inline so the loader
            // still works, just without the concurrency.
            if ( remote )
            {
                run_remote_request( request );
            }
            else
            {
                run_local_request( request );
            }
            return true;
        }

        request->taskId = pool->add_native_task(
            remote ? &run_remote_request : &run_local_request, request, false,
            godot::String( remote ? "tiles3d http fetch+decode" : "tiles3d file read+decode" ) );
        return true;
    }

    bool TilesetContentLoader::fetch_http( Request *request )
    {
        const std::string url = request->source.utf8().get_data();
        const core::UrlParts parts = core::splitUrl( url );
        if ( !parts.valid )
        {
            request->fetchError = godot::vformat( "cannot parse '%s' as a url", request->source );
            return false;
        }

        const godot::String host( parts.host.c_str() );

        // Kept from the pre-refactor accessor: resolving "localhost" is unreliable in some
        // sandboxed environments where the loopback name is not in the resolver.
        const godot::String connectHost =
            ( host == "localhost" ) ? godot::String( "127.0.0.1" ) : host;

        godot::Ref<godot::HTTPClient> client;
        client.instantiate();

        godot::Error err = client->connect_to_host( connectHost, parts.port );
        if ( err != godot::OK )
        {
            request->fetchError = godot::vformat( "cannot connect to '%s' (error %d)", host,
                                                  static_cast<int>( err ) );
            return false;
        }

        // Poll until the connection is up. This runs on a worker thread, so blocking here
        // costs the main thread nothing - which is the whole reason the fetch lives here.
        godot::HTTPClient::Status status = client->get_status();
        while ( status == godot::HTTPClient::STATUS_CONNECTING ||
                status == godot::HTTPClient::STATUS_RESOLVING )
        {
            if ( request->cancelled.load( std::memory_order_acquire ) )
            {
                return false;
            }
            client->poll();
            status = client->get_status();
        }

        if ( status != godot::HTTPClient::STATUS_CONNECTED )
        {
            request->fetchError = godot::vformat( "cannot connect to '%s' (status %d)", host,
                                                  static_cast<int>( status ) );
            return false;
        }

        const godot::String path( parts.path.c_str() );
        err = client->request( godot::HTTPClient::METHOD_GET, path, godot::PackedStringArray() );
        if ( err != godot::OK )
        {
            request->fetchError = godot::vformat( "cannot request '%s' (error %d)", request->source,
                                                  static_cast<int>( err ) );
            return false;
        }

        while ( client->get_status() == godot::HTTPClient::STATUS_REQUESTING )
        {
            if ( request->cancelled.load( std::memory_order_acquire ) )
            {
                return false;
            }
            client->poll();
        }

        if ( !client->has_response() )
        {
            request->fetchError = godot::vformat( "'%s' produced no response", request->source );
            return false;
        }

        godot::PackedByteArray body;
        while ( client->get_status() == godot::HTTPClient::STATUS_BODY )
        {
            if ( request->cancelled.load( std::memory_order_acquire ) )
            {
                return false;
            }
            client->poll();
            const godot::PackedByteArray chunk = client->read_response_body_chunk();
            if ( chunk.size() != 0 )
            {
                body.append_array( chunk );
            }
        }

        const int code = client->get_response_code();
        if ( code < 200 || code >= 300 )
        {
            request->fetchError = godot::vformat( "HTTP %d for '%s'", code, request->source );
            return false;
        }

        if ( body.size() == 0 )
        {
            request->fetchError = godot::vformat( "'%s' returned an empty body", request->source );
            return false;
        }

        request->bytes = body;
        return true;
    }

    void TilesetContentLoader::cancel( core::Tile *tile )
    {
        Request *request = find_request( tile );
        if ( request == nullptr )
        {
            return;
        }

        request->cancelled.store( true, std::memory_order_release );

        // A running worker cannot be stopped, but it checks `cancelled` and returns without
        // publishing. The record stays until it is drained, so the pointer it holds is valid.
    }

    void TilesetContentLoader::cancel_before_generation( std::uint64_t generation )
    {
        for ( const std::unique_ptr<Request> &request : requests_ )
        {
            if ( request->generation < generation )
            {
                request->cancelled.store( true, std::memory_order_release );
            }
        }
    }

    void TilesetContentLoader::bump_generation()
    {
        ++generation_;
    }

    std::uint64_t TilesetContentLoader::get_generation() const
    {
        return generation_;
    }

    void TilesetContentLoader::publish( Request *request, LoadState state,
                                       const godot::String &error )
    {
        CompletedLoad load;
        load.tile = request->tile;
        load.source = request->source;
        load.generation = request->generation;
        load.state = state;
        load.byteCount = static_cast<std::size_t>( request->bytes.size() );
        load.prepared = std::move( request->prepared );
        load.error = error;

        completed_.push_back( std::move( load ) );

        // Unlink. The worker is finished (or was cancelled), so the record can go.
        requests_.erase( std::remove_if( requests_.begin(), requests_.end(),
                                         [request]( const std::unique_ptr<Request> &r )
                                         { return r.get() == request; } ),
                         requests_.end() );
    }

    std::vector<CompletedLoad> TilesetContentLoader::collect_completed()
    {
        // Reap finished workers. Deliberately done here, on the main thread, rather than from
        // the worker: publishing into `completed_` from a worker would need a lock and would
        // let the engine call back into a tile the traversal is mid-way through.
        for ( std::size_t i = 0; i < requests_.size(); )
        {
            Request *request = requests_[i].get();

            if ( !request->finished.load( std::memory_order_acquire ) )
            {
                ++i;
                continue;
            }

            // The worker has returned. Reclaim the pool slot so it can be reused.
            if ( request->taskId != -1 )
            {
                godot::WorkerThreadPool *pool = godot::WorkerThreadPool::get_singleton();
                if ( pool != nullptr )
                {
                    pool->wait_for_task_completion( request->taskId );
                }
                request->taskId = -1;
            }

            if ( !request->fetchError.is_empty() )
            {
                publish( request, LoadState::Failed, request->fetchError );
            }
            else if ( !request->prepareError.is_empty() )
            {
                publish( request, LoadState::Failed, request->prepareError );
            }
            else if ( request->prepared == nullptr )
            {
                publish( request, LoadState::Failed, godot::String( "no content produced" ) );
            }
            else
            {
                publish( request, LoadState::Ready, godot::String() );
            }

            // publish() erased index i, so do not advance.
        }

        return std::move( completed_ );
    }

    void TilesetContentLoader::drain()
    {
        godot::WorkerThreadPool *pool = godot::WorkerThreadPool::get_singleton();

        for ( const std::unique_ptr<Request> &request : requests_ )
        {
            request->cancelled.store( true, std::memory_order_release );

            if ( request->taskId != -1 && pool != nullptr )
            {
                pool->wait_for_task_completion( request->taskId );
                request->taskId = -1;
            }
        }

        requests_.clear();
    }

} // namespace tiles3d
