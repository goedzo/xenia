project_root = "../../.."
include(project_root.."/tools/build")

project("xenia-base")
  uuid("aeadaf22-2b20-4941-b05f-a802d5679c11")
  kind("StaticLib")
  language("C++")
  links({
    "libarchive",
  })
  defines({
  })
  includedirs({
    project_root.."/third_party/gflags/src",
    project_root.."/third_party/libarchive/libarchive",
  })
  local_platform_files()
  removefiles({"main_*.cc"})
  files({
    "debug_visualizers.natvis",
  })
  resincludedirs({
    project_root,
  })

include("testing")
