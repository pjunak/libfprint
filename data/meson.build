if udev_hwdb_dir != ''
    # This file has to be updated using
    # ninja -C <builddir> libfprint/sync-udev-hwdb
    # Note that the unsupported device list needs to be manually synced from
    # the wiki. See comment in libfprint/fprint-list-uev-hwdb.c

    install_data('autosuspend.hwdb',
        rename: '60-autosuspend-@0@.hwdb'.format(versioned_libname),
        install_dir: udev_hwdb_dir,
    )
endif

build_info = configuration_data()
build_info.set_quoted('VERSION', meson.project_version())
build_info.set_quoted('BUILD_ID', get_option('build-id'))
build_info.set('SIGFM', have_sigfm ? 'true' : 'false')
build_info.set_quoted('DRIVERS', ','.join(drivers))
configure_file(input: 'build-info.json.in', output: 'build-info.json',
    configuration: build_info, install_dir: datadir / 'libfprint-goodix')
