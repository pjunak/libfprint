# Keep the internal C interface available in NBIS-only builds; public ABI and
# stored type numbers remain unchanged. Disabled operations fail explicitly.
have_sigfm = false
if sigfm_cpp
  opencv = dependency('opencv4', required: false)
  if not opencv.found()
    opencv = dependency('opencv5', required: get_option('sigfm'))
  endif
  have_sigfm = opencv.found()
endif
libfprint_conf.set10('HAVE_SIGFM', have_sigfm)
if have_sigfm
  libsigfm = static_library('sigfm', 'sigfm.cpp', dependencies: opencv)
  doctest = dependency('doctest', required: false, version: ['>=2.0.0', '<3.0.0'])
  if doctest.found()
    sigfm_tests = executable('sigfm-tests', 'tests.cpp', dependencies: [doctest, opencv], link_with: libsigfm)
    test('sigfm-legacy', sigfm_tests, suite: ['unit-tests'])
  endif
else
  libsigfm = static_library('sigfm', 'disabled.c')
endif
