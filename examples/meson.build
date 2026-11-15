if feature_conf.get('CAIRO_HAS_VULKAN_SURFACE', 0) == 1
  executable('cairo-vulkan-surface',
    'vulkan-surface.c',
    include_directories: [incbase],
    dependencies: [libcairo_dep],
    install: false,
  )
endif
