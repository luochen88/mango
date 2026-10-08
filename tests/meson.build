if anland_dep.found()
  mango_test_deps = [
    libm,
    libdrm,
    libscenefx_dep,
    wayland_server_dep,
    wlroots_dep,
    xkbcommon_dep,
    libinput_dep,
    libwayland_client_dep,
    pcre2_dep,
    pixman_dep,
    cjson_dep,
    pangocairo_dep,
    anland_dep,
  ]

  anland_backend_test = executable(
    'anland-backend-test',
    'anland_backend_test.c',
    wayland_sources,
    include_directories : mango_inc,
    dependencies : mango_test_deps,
    c_args : c_args + ['-ffunction-sections', '-fdata-sections'],
    link_args : link_args + ['-Wl,--gc-sections'],
  )
  test('anland-backend', anland_backend_test)

  text_input_test = executable(
    'text-input-test',
    'text_input_test.c',
    wayland_sources,
    include_directories : mango_inc,
    dependencies : mango_test_deps,
    c_args : c_args + ['-ffunction-sections', '-fdata-sections'],
    link_args : link_args + ['-Wl,--gc-sections'],
  )
  test('text-input', text_input_test)
endif
