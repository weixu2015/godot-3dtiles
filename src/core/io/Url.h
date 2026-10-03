// SPDX-License-Identifier: Unlicense
//
// URL / URI classification and joining.
//
// 3D Tiles content URIs are relative to the document that declared them, and a document can
// be reached over http(s) as well as from disk. The rules live here, in the engine-agnostic
// kernel, for two reasons:
//
//   1. they are pure string logic and can be unit tested without an engine, and
//   2. the Godot-side reader must not re-implement them - the path arithmetic that decides
//      whether a payload is local or remote has to be the same one that decides how to
//      fetch the document in the first place.

#ifndef TILES3D_CORE_IO_URL_H
#define TILES3D_CORE_IO_URL_H

#include <string>

namespace tiles3d::core
{
    /// True for the schemes that must be fetched over the network.
    ///
    /// Only http/https are recognised. Godot's FileAccess additionally understands res://
    /// and user://, but those are engine-virtual and are resolved by FileAccess itself, so
    /// they are deliberately not treated as remote here.
    bool isRemoteUrl( const std::string &url );

    /// True when the URI carries its own scheme and therefore must not be joined with a base.
    ///
    /// Covers the network schemes plus `data:` (base64 payloads) and `file:`. A Windows drive
    /// letter ("C:/...") is *not* a scheme, so it is excluded explicitly.
    bool hasUrlScheme( const std::string &uri );

    /// Joins a possibly-relative reference onto a base document URL.
    ///
    /// Works for both http(s) URLs and local paths, because the two differ only in the
    /// separator and in how the parent directory is derived. A reference that already carries
    /// a scheme is returned unchanged.
    ///
    /// The result is intentionally *not* normalised beyond `.` and `..` removal: re-encoding
    /// a URL risks changing its meaning, and 3D Tiles URIs are conventionally plain paths.
    std::string resolveUrl( const std::string &baseDocumentUrl, const std::string &reference );

    /// The directory part of a document URL, used as the base for its relative references.
    ///
    /// For "http://host/a/b/tileset.json" this is "http://host/a/b"; for "C:/a/b/tileset.json"
    /// it is "C:/a/b". The returned value never ends with a separator.
    std::string urlDirectory( const std::string &documentUrl );

    /// Collapses "." and ".." segments and duplicate separators in a *path* (no scheme, no
    /// authority). Kept separate from resolveUrl so the authority of a URL is never walked
    /// over by a ".." climb.
    std::string normalizePath( const std::string &path );

    /// An http(s) URL split into the parts a client needs.
    struct UrlParts
    {
        /// "http" or "https"; empty when the input had no scheme.
        std::string scheme;

        /// Host, without the port. Empty when the input had no authority.
        std::string host;

        /// Port, or -1 when the URL did not state one. Defaults (80 / 443) are *not* filled
        /// in: the caller passes -1 through to the client and lets it apply the scheme
        /// default, which keeps this function free of protocol assumptions.
        int port = -1;

        /// Path including the leading separator, and any query. "/" when the URL is bare.
        std::string path;

        /// True when every field a GET needs is present.
        bool valid = false;
    };

    /// Splits an absolute URL into host / port / path.
    ///
    /// Needed because Godot's HTTPClient::connect_to_host takes a host and a port, not a URL,
    /// so the split has to happen somewhere - and doing it here makes it unit testable and
    /// keeps the "does this URL have an authority" logic next to the rest of the URL rules.
    UrlParts splitUrl( const std::string &url );

} // namespace tiles3d::core

#endif // TILES3D_CORE_IO_URL_H
