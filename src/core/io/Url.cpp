// SPDX-License-Identifier: Unlicense

#include "io/Url.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace tiles3d::core
{
    namespace
    {
        /// Lowercases just enough ASCII for a scheme comparison. The scheme is always ASCII.
        std::string toLowerAscii( const std::string &text )
        {
            std::string lowered = text;
            std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                            []( unsigned char c )
                            { return static_cast<char>( std::tolower( c ) ); } );
            return lowered;
        }

        /// "http://" and friends. The reference is case-insensitive per RFC 3986.
        bool startsWithScheme( const std::string &uri, const char *scheme )
        {
            const std::size_t length = std::char_traits<char>::length( scheme );
            if ( uri.size() < length + 1 )
            {
                return false;
            }
            if ( toLowerAscii( uri.substr( 0, length ) ) != scheme )
            {
                return false;
            }
            // The scheme must be followed by its colon, not by more scheme characters.
            return uri[length] == ':';
        }

        /// Splits a URL into "scheme://authority" and the path that follows it.
        ///
        /// For URLs there is no notion of "the parent of this directory" above the authority,
        /// and stripping a separator while climbing can eat the two slashes of "//host". So
        /// the authority is held aside and rejoined verbatim.
        // Named distinctly from the public core::UrlParts / core::splitUrl below: this one
        // keeps the authority aside for path joining, that one feeds HTTPClient.
        struct JoinParts
        {
            std::string prefix; ///< includes the "//" and authority, empty for local paths
            std::string path;   ///< the part that may be manipulated
            bool remote = false;
        };

        JoinParts splitForJoin( const std::string &url )
        {
            JoinParts parts;

            const std::size_t schemeEnd = url.find( ':' );
            if ( schemeEnd == std::string::npos )
            {
                parts.path = url;
                return parts;
            }

            const std::string scheme = toLowerAscii( url.substr( 0, schemeEnd ) );
            const bool isNetworkScheme = ( scheme == "http" || scheme == "https" );

            if ( !isNetworkScheme )
            {
                // file:// and friends: the whole thing is a path once the scheme is gone, and
                // a Windows drive letter is not a scheme at all.
                parts.path = url;
                if ( scheme == "file" )
                {
                    std::size_t start = schemeEnd + 1;
                    while ( start < url.size() && url[start] == '/' )
                    {
                        ++start;
                    }
                    parts.path = url.substr( start );
                }
                return parts;
            }

            std::size_t authorityStart = schemeEnd + 1;
            if ( url.compare( authorityStart, 2, "//" ) != 0 )
            {
                // "http:path" - unusual but legal; there is no authority to preserve.
                parts.path = url.substr( authorityStart );
                parts.remote = true;
                return parts;
            }

            const std::size_t pathStart = url.find( '/', authorityStart + 2 );
            if ( pathStart == std::string::npos )
            {
                parts.prefix = url;
                parts.path.clear();
                parts.remote = true;
                return parts;
            }

            parts.prefix = url.substr( 0, pathStart );
            parts.path = url.substr( pathStart );
            parts.remote = true;
            return parts;
        }

        constexpr char kSeparator = '/';

        std::string parentDirectory( const std::string &path )
        {
            const std::size_t slash = path.find_last_of( kSeparator );
            if ( slash == std::string::npos )
            {
                return std::string();
            }
            return path.substr( 0, slash );
        }
    } // namespace

    bool isRemoteUrl( const std::string &url )
    {
        return startsWithScheme( url, "http" ) || startsWithScheme( url, "https" );
    }

    bool hasUrlScheme( const std::string &uri )
    {
        const std::size_t colon = uri.find( ':' );
        if ( colon == std::string::npos || colon == 0 )
        {
            return false;
        }

        // A Windows drive letter ("C:/...") looks like a one-character scheme. Require at
        // least two scheme characters so it is not mistaken for one.
        if ( colon == 1 )
        {
            return false;
        }

        // RFC 3986 allows letters, digits, '+', '-' and '.' in a scheme.
        for ( std::size_t i = 0; i < colon; ++i )
        {
            const unsigned char c = static_cast<unsigned char>( uri[i] );
            const bool valid = std::isalpha( c ) != 0 || std::isdigit( c ) != 0 || c == '+' ||
                               c == '-' || c == '.';
            if ( !valid )
            {
                return false;
            }
        }

        // The first character must be a letter.
        return std::isalpha( static_cast<unsigned char>( uri[0] ) ) != 0;
    }

    std::string urlDirectory( const std::string &documentUrl )
    {
        const JoinParts parts = splitForJoin( documentUrl );
        return parts.prefix + parentDirectory( parts.path );
    }

    std::string resolveUrl( const std::string &baseDocumentUrl, const std::string &reference )
    {
        if ( reference.empty() )
        {
            return baseDocumentUrl;
        }

        // A reference with its own scheme replaces the base entirely.
        if ( hasUrlScheme( reference ) )
        {
            return reference;
        }

        const JoinParts base = splitForJoin( baseDocumentUrl );
        const std::string directory = parentDirectory( base.path );

        // A URL authority is followed by a separator: "http://host" + "/" + path. A local
        // path has no authority, so the prefix is empty and this is a no-op. Without it, a
        // chain of ".." that empties the path would yield "http://hostx.b3dm".
        const bool needsAbsolute = !base.prefix.empty();

        // Absolute path on the same authority: keep the authority, replace the path.
        if ( !reference.empty() && reference[0] == kSeparator )
        {
            return base.prefix + normalizePath( reference );
        }

        const std::string joined =
            directory.empty() ? reference : directory + kSeparator + reference;

        std::string normalized = normalizePath( joined );
        if ( needsAbsolute && ( normalized.empty() || normalized[0] != kSeparator ) )
        {
            normalized.insert( normalized.begin(), kSeparator );
        }

        return base.prefix + normalized;
    }

    std::string normalizePath( const std::string &path )
    {
        // Segment-stack implementation, deliberately not a character state machine: the
        // leading-separator and ".."-below-root cases are the ones that bite, and they are
        // trivial to get right when the segments are held in a vector.
        const bool absolute = !path.empty() && path[0] == kSeparator;

        std::vector<std::string> segments;
        std::size_t index = 0;
        const std::size_t length = path.size();

        while ( index < length )
        {
            // Skip the separator run; one separator is implied between every output segment.
            if ( path[index] == kSeparator )
            {
                ++index;
                continue;
            }

            std::size_t end = path.find( kSeparator, index );
            if ( end == std::string::npos )
            {
                end = length;
            }

            std::string segment = path.substr( index, end - index );
            index = end;

            if ( segment == "." )
            {
                continue;
            }

            if ( segment == ".." )
            {
                // Climb. At the root there is nothing left to pop, and the climb must be
                // dropped rather than allowed to consume the authority - the caller put the
                // authority aside precisely so this cannot happen.
                if ( !segments.empty() )
                {
                    segments.pop_back();
                }
                continue;
            }

            segments.push_back( std::move( segment ) );
        }

        std::string result;
        if ( absolute )
        {
            result.push_back( kSeparator );
        }

        for ( std::size_t i = 0; i < segments.size(); ++i )
        {
            if ( i > 0 )
            {
                result.push_back( kSeparator );
            }
            result += segments[i];
        }

        return result;
    }

    UrlParts splitUrl( const std::string &url )
    {
        UrlParts parts;

        const std::size_t schemeEnd = url.find( "://" );
        if ( schemeEnd == std::string::npos )
        {
            return parts;
        }

        parts.scheme = url.substr( 0, schemeEnd );

        // Authority ends at the first '/' after the scheme, or at the end of the string.
        const std::size_t authorityStart = schemeEnd + 3;
        const std::size_t pathStart = url.find( '/', authorityStart );

        std::string authority =
            url.substr( authorityStart, pathStart == std::string::npos
                                            ? std::string::npos
                                            : pathStart - authorityStart );

        parts.path = pathStart == std::string::npos ? "/" : url.substr( pathStart );
        if ( parts.path.empty() )
        {
            parts.path = "/";
        }

        // Userinfo ("user@host") is not needed; drop it rather than hand it to the client.
        const std::size_t at = authority.rfind( '@' );
        if ( at != std::string::npos )
        {
            authority = authority.substr( at + 1 );
        }

        // Reject an empty authority outright: "http:///x" has nothing to connect to.
        if ( authority.empty() )
        {
            return parts;
        }

        // IPv6 literals are bracketed and contain colons, so the port separator has to be
        // searched after the closing bracket rather than with a plain rfind.
        std::size_t colon = std::string::npos;
        if ( authority[0] == '[' )
        {
            const std::size_t close = authority.find( ']' );
            if ( close != std::string::npos )
            {
                colon = authority.find( ':', close );
            }
        }
        else
        {
            colon = authority.find( ':' );
        }

        if ( colon == std::string::npos )
        {
            parts.host = authority;
        }
        else
        {
            parts.host = authority.substr( 0, colon );

            const std::string portText = authority.substr( colon + 1 );
            if ( !portText.empty() )
            {
                int port = 0;
                bool digits = true;
                for ( const char c : portText )
                {
                    if ( c < '0' || c > '9' )
                    {
                        digits = false;
                        break;
                    }
                    port = port * 10 + ( c - '0' );
                }

                if ( digits && port > 0 && port <= 65535 )
                {
                    parts.port = port;
                }
            }
        }

        parts.valid = !parts.host.empty();
        return parts;
    }

    std::string encodeUrlPath( const std::string &path )
    {
        constexpr char kHex[] = "0123456789ABCDEF";

        auto isHexDigit = []( char c )
        {
            return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
        };

        auto isSafe = []( unsigned char c )
        {
            if ( ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) )
            {
                return true;
            }

            switch ( c )
            {
                // Unreserved.
                case '-':
                case '.':
                case '_':
                case '~':
                // Sub-delimiters that are legal in a path or a query.
                case '!':
                case '$':
                case '&':
                case '\'':
                case '(':
                case ')':
                case '*':
                case '+':
                case ',':
                case ';':
                case '=':
                // Separators that must stay literal: the path split and the query introducer.
                case ':':
                case '@':
                case '/':
                case '?':
                    return true;
                default:
                    return false;
            }
        };

        std::string encoded;
        encoded.reserve( path.size() + 8 );

        for ( std::size_t i = 0; i < path.size(); ++i )
        {
            const unsigned char c = static_cast<unsigned char>( path[i] );

            // Pass an existing escape through untouched rather than encoding the '%'. Encoding
            // it would turn "%20" into "%2520" and the server would look for a file literally
            // named "%20".
            if ( c == '%' && i + 2 < path.size() && isHexDigit( path[i + 1] ) &&
                 isHexDigit( path[i + 2] ) )
            {
                encoded.append( path, i, 3 );
                i += 2;
                continue;
            }

            if ( isSafe( c ) )
            {
                encoded.push_back( static_cast<char>( c ) );
                continue;
            }

            encoded.push_back( '%' );
            encoded.push_back( kHex[c >> 4] );
            encoded.push_back( kHex[c & 0x0F] );
        }

        return encoded;
    }

} // namespace tiles3d::core
