// SPDX-License-Identifier: Unlicense
//
// Tests for src/core/io/Url.h.
//
// These matter because the same resolution decides two things that must agree: how a
// document is fetched (local FileAccess vs HTTPRequest) and where its relative content URIs
// point. A wrong answer in one and not the other produces a tileset that loads over http and
// then silently fails to find any of its payloads.

#include <doctest/doctest.h>

#include "io/Url.h"

using tiles3d::core::encodeUrlPath;
using tiles3d::core::hasUrlScheme;
using tiles3d::core::isRemoteUrl;
using tiles3d::core::normalizePath;
using tiles3d::core::resolveUrl;
using tiles3d::core::splitUrl;
using tiles3d::core::urlDirectory;

TEST_CASE( "scheme classification separates remote from local" )
{
    CHECK( isRemoteUrl( "http://localhost:9090/3D Tiles/weinan/tileset.json" ) );
    CHECK( isRemoteUrl( "https://example.com/a/b.json" ) );
    CHECK( isRemoteUrl( "HTTP://example.com/a.json" ) );

    CHECK_FALSE( isRemoteUrl( "C:/data/tileset.json" ) );
    CHECK_FALSE( isRemoteUrl( "/home/user/tileset.json" ) );
    CHECK_FALSE( isRemoteUrl( "res://demo/tileset.json" ) );
    CHECK_FALSE( isRemoteUrl( "file:///C:/data/tileset.json" ) );
    CHECK_FALSE( isRemoteUrl( "httpnotascheme/x" ) );
}

TEST_CASE( "a windows drive letter is not a url scheme" )
{
    // The single most important false positive: "C:" looks exactly like a scheme.
    CHECK_FALSE( hasUrlScheme( "C:/data/tileset.json" ) );
    CHECK_FALSE( hasUrlScheme( "d:/x" ) );

    CHECK( hasUrlScheme( "http://a" ) );
    CHECK( hasUrlScheme( "https://a" ) );
    CHECK( hasUrlScheme( "data:application/octet-stream;base64,AAAA" ) );
    CHECK( hasUrlScheme( "file:///C:/x" ) );

    CHECK_FALSE( hasUrlScheme( "a/b/c.json" ) );
    CHECK_FALSE( hasUrlScheme( "/abs/path.json" ) );
}

TEST_CASE( "urlDirectory keeps the authority for http and the drive for local" )
{
    CHECK( urlDirectory( "http://localhost:9090/3D Tiles/weinan/tileset.json" ) ==
           "http://localhost:9090/3D Tiles/weinan" );
    CHECK( urlDirectory( "https://example.com/tileset.json" ) == "https://example.com" );
    CHECK( urlDirectory( "C:/data/weinan/tileset.json" ) == "C:/data/weinan" );
    CHECK( urlDirectory( "res://demo/x/tileset.json" ) == "res://demo/x" );
}

TEST_CASE( "relative content uris resolve against the document" )
{
    // The reported failure: a tileset served over http whose payloads must also come over http.
    const std::string base = "http://localhost:9090/3D Tiles/weinan/tileset.json";

    CHECK( resolveUrl( base, "tiles/0.b3dm" ) ==
           "http://localhost:9090/3D Tiles/weinan/tiles/0.b3dm" );
    CHECK( resolveUrl( base, "./tiles/0.b3dm" ) ==
           "http://localhost:9090/3D Tiles/weinan/tiles/0.b3dm" );
    CHECK( resolveUrl( base, "../shared/0.b3dm" ) ==
           "http://localhost:9090/3D Tiles/shared/0.b3dm" );

    // Local mirror of the same shape must keep its drive letter and back-to-front slashes.
    CHECK( resolveUrl( "C:/data/weinan/tileset.json", "tiles/0.b3dm" ) ==
           "C:/data/weinan/tiles/0.b3dm" );
    CHECK( resolveUrl( "C:/data/weinan/tileset.json", "../shared/0.b3dm" ) ==
           "C:/data/shared/0.b3dm" );
}

TEST_CASE( "an absolute reference keeps the authority but replaces the path" )
{
    const std::string base = "http://localhost:9090/3D Tiles/weinan/tileset.json";

    CHECK( resolveUrl( base, "/other/0.b3dm" ) == "http://localhost:9090/other/0.b3dm" );
}

TEST_CASE( "a reference carrying its own scheme is not joined" )
{
    const std::string base = "C:/data/weinan/tileset.json";

    CHECK( resolveUrl( base, "https://cdn.example.com/0.b3dm" ) ==
           "https://cdn.example.com/0.b3dm" );
}

TEST_CASE( "a parent climb never eats the url authority" )
{
    // Regression guard: normalisePath is applied to the path only, so ".." must stop at the
    // root. If the authority were part of the normalised string, this would produce
    // "http:/localhost/x" or similar and every request would fail.
    const std::string base = "http://localhost:9090/tileset.json";

    CHECK( resolveUrl( base, "../x.b3dm" ) == "http://localhost:9090/x.b3dm" );
    CHECK( resolveUrl( base, "../../../../x.b3dm" ) == "http://localhost:9090/x.b3dm" );
}

TEST_CASE( "normalizePath collapses dots and separators" )
{
    CHECK( normalizePath( "/a/b/../c" ) == "/a/c" );
    CHECK( normalizePath( "/a/./b" ) == "/a/b" );
    CHECK( normalizePath( "/a//b" ) == "/a/b" );
    CHECK( normalizePath( "/a/b/../../c" ) == "/c" );
    CHECK( normalizePath( "relative/x" ) == "relative/x" );
}

TEST_CASE( "splitUrl separates the parts connect_to_host needs" )
{
    // Regression guard for the http fix: Godot's HTTPClient::connect_to_host takes a host and
    // a port, not a URL. Handing it the whole URL fails with STATUS_CANT_RESOLVE, which is
    // exactly what the first version of the loader did.
    const auto withPort = splitUrl( "http://localhost:9090/weinan/tileset.json" );
    CHECK( withPort.valid );
    CHECK( withPort.scheme == "http" );
    CHECK( withPort.host == "localhost" );
    CHECK( withPort.port == 9090 );
    CHECK( withPort.path == "/weinan/tileset.json" );

    const auto defaultPort = splitUrl( "https://example.com/a/b.glb" );
    CHECK( defaultPort.valid );
    CHECK( defaultPort.scheme == "https" );
    CHECK( defaultPort.host == "example.com" );
    // -1, not 443: the client applies the scheme default itself.
    CHECK( defaultPort.port == -1 );
    CHECK( defaultPort.path == "/a/b.glb" );

    const auto bare = splitUrl( "http://example.com" );
    CHECK( bare.valid );
    CHECK( bare.host == "example.com" );
    CHECK( bare.path == "/" );

    // A path with a space - the real dataset directory is literally "3D Tiles".
    const auto spaced = splitUrl( "http://127.0.0.1:9090/3D%20Tiles/weinan/tileset.json" );
    CHECK( spaced.valid );
    CHECK( spaced.host == "127.0.0.1" );
    CHECK( spaced.port == 9090 );
    CHECK( spaced.path == "/3D%20Tiles/weinan/tileset.json" );
}

TEST_CASE( "splitUrl rejects what cannot be fetched" )
{
    // No scheme at all: a local path, not a URL.
    CHECK_FALSE( splitUrl( "C:/data/weinan/tileset.json" ).valid );

    // Scheme but empty authority: nothing to connect to.
    CHECK_FALSE( splitUrl( "http:///x" ).valid );

    // A non-numeric port is ignored rather than misparsed, and the host still comes through.
    const auto badPort = splitUrl( "http://host:notaport/x" );
    CHECK( badPort.host == "host" );
    CHECK( badPort.port == -1 );
}

TEST_CASE( "splitUrl handles userinfo and IPv6 authorities" )
{
    const auto userinfo = splitUrl( "http://user:pass@host:8080/a" );
    CHECK( userinfo.host == "host" );
    CHECK( userinfo.port == 8080 );

    // An IPv6 literal is bracketed and contains colons, so the port separator must be found
    // after the closing bracket rather than by a plain search for ':'.
    const auto ipv6 = splitUrl( "http://[::1]:9090/a" );
    CHECK( ipv6.host == "[::1]" );
    CHECK( ipv6.port == 9090 );
}

TEST_CASE( "encodeUrlPath makes a dataset path requestable without double-encoding" )
{
    // The datasets on this machine live under "3D Tiles/". Unencoded, the space ends the
    // request target and the server answers a request for "/3D" with 404.
    CHECK( encodeUrlPath( "/3D Tiles/weinan/tileset.json" ) ==
           "/3D%20Tiles/weinan/tileset.json" );

    // An escape the author already wrote has to survive: encoding the '%' would turn "%20"
    // into "%2520" and the server would look for a file literally named "%20".
    CHECK( encodeUrlPath( "/3D%20Tiles/a.json" ) == "/3D%20Tiles/a.json" );
    CHECK( encodeUrlPath( "/a%2Fb" ) == "/a%2Fb" );

    // Query syntax and the separators that may stay literal are untouched.
    CHECK( encodeUrlPath( "/tiles/x.jpeg?n=z&g=11404" ) == "/tiles/x.jpeg?n=z&g=11404" );
    CHECK( encodeUrlPath( "/a-b_c.d~e:f@g;h+i,j(k)l'm!n$o*p=q" ) ==
           "/a-b_c.d~e:f@g;h+i,j(k)l'm!n$o*p=q" );

    // Characters that would break out of the request target are escaped.
    CHECK( encodeUrlPath( "/a\"b<c>d\\e^f`g|h{i}j" ) ==
           "/a%22b%3Cc%3Ed%5Ce%5Ef%60g%7Ch%7Bi%7Dj" );

    // Non-ASCII arrives as UTF-8 bytes and is escaped one byte at a time: 中 is E4 B8 AD.
    CHECK( encodeUrlPath( "/\xe4\xb8\xad" ) == "/%E4%B8%AD" );

    // A trailing '%' that is not an escape is escaped itself, and empty stays empty.
    CHECK( encodeUrlPath( "/a%" ) == "/a%25" );
    CHECK( encodeUrlPath( "" ) == "" );

    // Round trip through splitUrl: this is the exact pair the loader uses.
    const auto parts = splitUrl( "http://localhost:9090/3D Tiles/weinan/tileset.json" );
    CHECK( parts.host == "localhost" );
    CHECK( parts.port == 9090 );
    CHECK( encodeUrlPath( parts.path ) == "/3D%20Tiles/weinan/tileset.json" );
}
