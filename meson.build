
project('str2dbl', 'c')

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)

str2dbl_lib = static_library('str2dbl',
  'str2dbl.c'
)

test_str2dbl_exe = executable('test-str2dbl',
  'test-str2dbl.c',
  link_with : [str2dbl_lib],
  dependencies : [m_dep],
)

test('str2dbl',
  test_str2dbl_exe,
  args : ['test-str2dbl.txt'],
  workdir : meson.current_source_dir(),
)
