add_executable(installer_core_tests test_main.cpp ../src/ui/NativeStrings.cpp)
target_include_directories(installer_core_tests PRIVATE ../resources)
if(WIN32)
  add_executable(folder_cleanup_tests test_folder_cleanup.cpp)
  target_link_libraries(folder_cleanup_tests PRIVATE installer_core advapi32)
  add_test(NAME folder_cleanup_tests COMMAND folder_cleanup_tests)

  add_executable(smoke_script_driver smoke_script_driver.cpp)
  target_link_libraries(smoke_script_driver PRIVATE installer_core)

  target_sources(installer_core_tests PRIVATE ../resources/app.rc)
endif()
target_link_libraries(installer_core_tests PRIVATE installer_core)
add_test(NAME installer_core_tests COMMAND installer_core_tests)
set_tests_properties(installer_core_tests PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_executable(pipeline_scheduler_tests test_pipeline_scheduler.cpp)
target_include_directories(pipeline_scheduler_tests PRIVATE ../src)
target_compile_features(pipeline_scheduler_tests PRIVATE cxx_std_20)
add_test(NAME pipeline_scheduler_tests COMMAND pipeline_scheduler_tests)

if(WIN32)
  add_executable(pipeline_smoke pipeline_smoke.cpp ../resources/app.rc)
  target_include_directories(pipeline_smoke PRIVATE ../resources)
  target_link_libraries(pipeline_smoke PRIVATE installer_core)

  add_executable(installer_ui_tests test_ui.cpp ../src/ui/NativeInstallerView.cpp
    ../src/ui/NativeStrings.cpp ../resources/app.rc)
  target_include_directories(installer_ui_tests PRIVATE ../resources)
  if(MSVC)
    target_compile_options(installer_ui_tests PRIVATE /utf-8)
  endif()
  target_link_libraries(installer_ui_tests PRIVATE installer_core
    user32 gdi32 d2d1 dwrite dwmapi uxtheme comdlg32 shell32 ole32 oleaut32 comctl32 shlwapi advapi32)
  add_test(NAME installer_ui_tests COMMAND installer_ui_tests)

  add_executable(installer_workflow_tests test_install_workflow.cpp ../src/ui/NativeInstallerView.cpp
    ../src/ui/NativeStrings.cpp ../resources/app.rc)
  target_include_directories(installer_workflow_tests PRIVATE ../resources)
  if(MSVC)
    target_compile_options(installer_workflow_tests PRIVATE /utf-8)
  endif()
  target_link_libraries(installer_workflow_tests PRIVATE installer_core
    user32 gdi32 d2d1 dwrite dwmapi uxtheme comdlg32 shell32 ole32 oleaut32 comctl32 shlwapi advapi32)

  if(MSVC)
    # These executables embed the application manifest through app.rc.
    foreach(target installer_core_tests pipeline_smoke installer_ui_tests installer_workflow_tests)
      target_link_options(${target} PRIVATE /MANIFEST:NO)
    endforeach()
  endif()
endif()
