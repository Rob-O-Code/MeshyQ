# MeshyQ Web

A browser-based GUI for controlling a [MeshyQ](../README.md) node over USB,
using the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API)
to talk to the node's serial shell directly from the browser — no native app
or drivers required.

This is currently an empty scaffold (Vite + TypeScript); Web Serial
integration is not implemented yet.

## Setup

```sh
npm install
```

## Development

```sh
npm run dev
```

## Build

```sh
npm run build
```

Bundles to a single self-contained `dist/index.html` (via
`vite-plugin-singlefile`), suitable for serving as a static site (e.g.
GitHub Pages) or loading directly in a minimal-overhead Android WebView
wrapper.
