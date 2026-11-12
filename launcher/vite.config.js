import { defineConfig } from 'vite';

// Tauri expects a fixed dev port and must not have Vite watch the Rust side.
export default defineConfig({
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    watch: { ignored: ['**/src-tauri/**'] },
    // settings-schema.js imports the cheats overlay's look spec from the runtime (runtime/assets/cheats/look.json): one source for both
    fs: { allow: ['..'] },
  },
  build: {
    target: 'es2022', // WebView2 is Chromium
    outDir: 'dist',
  },
});
