# Third-party headers

Vendored unchanged so the relay builds without system headers; each file keeps its
license notice.

- `include/EGL`, `include/GLES2`, `include/KHR`: Khronos Group API headers
  (MIT or Apache-2.0, per file; https://github.com/KhronosGroup/EGL-Registry,
  https://github.com/KhronosGroup/OpenGL-Registry).
- `include/wayland-*.h`: Wayland client and wayland-egl headers (MIT;
  https://gitlab.freedesktop.org/wayland/wayland).
