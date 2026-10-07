
examples = [
    'enroll',
    'identify',
    'img-capture',
    'manage-prints',
    'verify',
]

foreach example: examples
    executable(example,
        [ example + '.c', 'storage.c', 'utilities.c' ],
        dependencies: [
            libfprint_dep,
            glib_dep,
        ],
    )
endforeach

if sigfm_cpp
executable('cpp-test',
    'cpp-test.cpp',
    dependencies: libfprint_dep,
)

endif

if installed_tests
    install_subdir('prints',
        install_dir: installed_tests_testdir)
endif
