# One prompt that accepts the password or a fingerprint (sudo, polkit). It
# talks to fprintd over D-Bus and does not link libfprint.
pam_dep = dependency('pam', required: false)
libsystemd_dep = dependency('libsystemd', required: false)
pam_parallel_enabled = pam_dep.found() and libsystemd_dep.found()
if pam_parallel_enabled
    pam_fprint_parallel = shared_module('pam_fprint_parallel', 'pam_fprint_parallel.c',
        name_prefix: '',
        dependencies: [pam_dep, libsystemd_dep, dependency('threads')],
        gnu_symbol_visibility: 'hidden',
        # In conversation mode a thread can still be inside the module after
        # pam_end(); libpam must not unmap the code underneath it.
        link_args: ['-Wl,-z,nodelete'],
        install: true,
        install_dir: libdir / 'security')
endif
