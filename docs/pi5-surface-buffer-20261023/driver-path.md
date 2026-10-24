# Allocation path inspected in the matched Android17 tree

- frameworks camera stream dump: TextureView usage0x300; SurfaceView0xb00. Both1920x1080, RGBA8888.
- external/minigbm/cros_gralloc/cros_gralloc_helpers.cc:140 maps HW_COMPOSER to BO_USE_SCANOUT|BO_USE_TEXTURE.
- external/minigbm/gbm_mesa_driver/gbm_mesa_internals.cpp:361 sets use_scanout when BO_USE_SCANOUT is present; :359 only forces_linear for CPU software access.
- external/minigbm/gbm_mesa_driver/gbm_mesa_wrapper.c:90 maps use_scanout to GBM_BO_USE_SCANOUT and calls gbm_bo_create without explicit modifier negotiation.
- external/mesa3d/src/gallium/drivers/v3d/v3d_resource.c:846-850 forces linear for legacy PIPE_BIND_SCANOUT because the API does not express supported display modifiers.
- external/libdrm/include/drm/drm_fourcc.h:1088 defines Broadcom UIF as vendor7,value6:0x0700000000000006; linear=0.
- HAL GpuCompositor::composite draws one texture-sampled full-screen quad into an imported destination; it is not just queueing the producer framebuffer.
- Mesa v3dx_draw.c:1054-1102 marks existing color attachment load unless clear/invalidate says it need not preserve contents. HAL currently neither clears nor invalidates before full-coverage output. A separate bounded clear/discard trial could test unnecessary destination reads; not changed by allocation diagnostics.
