# Applications built on the framework. They use only the installed headers and
# AMDVA:: targets, so each directory also configures on its own against an
# installed SDK (find_package(AMDVideoAnalysis)).
option(AMDVA_BUILD_DASHBOARD "Build the hardware-encoded RTSP resource dashboard" ON)
option(AMDVA_BUILD_RTSP_FILES "Build the file-to-RTSP test server" ON)
option(AMDVA_BUILD_NVR "Build the NVR sample backend and headless service" ON)
option(AMDVA_BUILD_NVR_GUI "Build the Qt desktop NVR (needs AMDVA_BUILD_NVR and Qt6)" ON)
if(AMDVA_BUILD_DASHBOARD)
  add_subdirectory(resource-dashboard)
endif()
if(AMDVA_BUILD_RTSP_FILES)
  add_subdirectory(rtsp-files)
endif()
if(AMDVA_BUILD_NVR)
  if(NOT AMDVA_BUILD_STREAMING)
    message(FATAL_ERROR "The NVR sample requires AMDVA_BUILD_STREAMING")
  endif()
  add_subdirectory(nvr)
endif()
