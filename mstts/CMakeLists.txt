cmake_minimum_required(VERSION 3.20)
project(mstts VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT APPLE)
  message(FATAL_ERROR "mstts 只支持 macOS（依赖 CoreAudio / AudioToolbox）")
endif()

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "最低系统版本" FORCE)
endif()

set(MS_SPEECH_DIR "${CMAKE_SOURCE_DIR}/third_party/ms-speech")
set(MS_SPEECH_LIBS
    "${MS_SPEECH_DIR}/lib/libMicrosoft.CognitiveServices.Speech.core.dylib"
    "${MS_SPEECH_DIR}/lib/libMicrosoft.CognitiveServices.Speech.extension.embedded.tts.dylib"
    "${MS_SPEECH_DIR}/lib/libMicrosoft.CognitiveServices.Speech.extension.onnxruntime.dylib")

foreach(lib ${MS_SPEECH_LIBS})
  if(NOT EXISTS "${lib}")
    message(FATAL_ERROR "缺少 ${lib}\n先运行：./scripts/fetch-sdk.sh")
  endif()
endforeach()

find_package(Threads REQUIRED)

add_library(mstts_core STATIC
  src/engine.cpp
  src/http_server.cpp
  src/json.cpp
  src/player.cpp
  src/ssml.cpp
  src/voices.cpp
  src/wav.cpp)

target_include_directories(mstts_core PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  "${MS_SPEECH_DIR}/include/cxx_api"
  "${MS_SPEECH_DIR}/include/c_api")

target_link_libraries(mstts_core PUBLIC
  ${MS_SPEECH_LIBS}
  "-framework AudioToolbox"
  "-framework CoreFoundation"
  Threads::Threads)

add_executable(mstts src/main.cpp)
target_link_libraries(mstts PRIVATE mstts_core)

# 运行期把三个 dylib 放到可执行文件旁的 lib/，rpath 用 @executable_path/lib，
# 这样 build 目录直接可以跑，拷走 build/mstts + build/lib 也能跑。
add_custom_command(TARGET mstts POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mstts>/lib"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MS_SPEECH_LIBS} "$<TARGET_FILE_DIR:mstts>/lib/"
  COMMENT "复制微软语音 SDK dylib 到 $<TARGET_FILE_DIR:mstts>/lib")

set_target_properties(mstts PROPERTIES
  BUILD_RPATH "@executable_path/lib"
  INSTALL_RPATH "@executable_path/../lib")

install(TARGETS mstts RUNTIME DESTINATION bin)
install(FILES ${MS_SPEECH_LIBS} DESTINATION lib)

# ---- 测试 ----
enable_testing()
add_executable(mstts_tests tests/unit.cpp)
target_link_libraries(mstts_tests PRIVATE mstts_core)
set_target_properties(mstts_tests PROPERTIES BUILD_RPATH "@executable_path/lib")
add_test(NAME unit COMMAND mstts_tests)
