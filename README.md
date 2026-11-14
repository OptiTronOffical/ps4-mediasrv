# PS4 Video Web Server

A video-library variant of PS4-Websrv. It serves video files from the PS4's
`/data/websrv` directory to phones, tablets, PCs and smart TVs on the same LAN.

## Features

- Browser-based media library with folder navigation and refresh.
- Streams video directly from the PS4 filesystem; files are not copied or loaded
  into memory in their entirety.
- HTTP `Range` / `206 Partial Content` support, `Content-Range`, and
  `Accept-Ranges: bytes` for seeking, scrubbing and resuming playback.
- `HEAD` support and correct MIME types for common video formats.
- Nested folders supported; directory listings show video files and folders.
- Path traversal and symlink escape protection for served files.
- Retains the original payload's PS4 network startup, IP notification,
  instance replacement and shutdown behavior.

## Install and use

1. Build `ps4-websrv-video.bin` using the PS4 Payload SDK (see below).
2. Load the payload on the PS4 using your usual payload loader.
3. Copy videos into `/data/websrv`. Subfolders are supported, for example:

   ```
   /data/websrv/
     index.html
     Movies/
       film.mp4
       clip.webm
     TV/
       episode.mp4
   ```

4. On a device connected to the same local network, open
   `http://PS4-IP/` (port 80).
5. Select a folder or video. Use the browser's normal playback controls to
   pause, seek, change volume or enter fullscreen.

The payload creates `/data/websrv` and installs its embedded media-library
`index.html` if no index file already exists. Existing files are preserved.
If an older custom `index.html` is already present, back it up and replace it
with the supplied `index.html` to use the new media browser.

## Supported formats

The server sends MIME types for MP4/M4V, WebM, Matroska (MKV), QuickTime MOV,
AVI, MPEG and transport-stream files. Actual playback depends on the receiving
device's browser and codec support. MP4 with H.264 video and AAC audio is the
most broadly compatible choice. Unsupported formats may still be downloadable
or playable in a compatible third-party player.

## Build

Install and configure [ps4-payload-sdk](https://github.com/Scene-Collective/ps4-payload-sdk/),
then run:

```sh
make
```

The output is `ps4-websrv-video.bin`. The build expects `PS4SDK` to point to the
SDK directory, as in the original project.

## Notes

- This is a local-network HTTP server, not an internet-facing or authenticated
  media service. Use it only on a trusted LAN; anyone who can reach the PS4
  server can request files under `/data/websrv`.
- Port 80 must be available on the PS4.
- Large files are streamed in chunks. Playback performance depends on Wi-Fi,
  storage speed, PS4 load and the receiving device.
- The server remains capped at the original 12 concurrent client connections.
- The existing `/shutdown` endpoint remains available to stop the server.
