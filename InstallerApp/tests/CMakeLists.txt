add_executable(installer_core_tests test_main.cpp ../src/ui/NativeStrings.cpp)
target_include_directories(installer_core_tests PRIVATE ../resources)
if(WIN32)
  target_sources(installer_core_tests PRIVATE ../resources/app.rc)
endif()
target_link_libraries(installer_core_tests PRIVATE installer_core)
add_test(NAME installer_core_tests COMMAND installer_core_tests)

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
endif()
