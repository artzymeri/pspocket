# Host services shared by the Apple platforms (macOS and iOS): sound out of cellAudio through AVAudioEngine. The iOS
# app compiles src/ too (app/ios/project.yml).
if(APPLE)
    enable_language(OBJCXX)
    add_library(pspocket_platform_apple STATIC src/audio_output.mm)
    target_include_directories(pspocket_platform_apple PUBLIC include)
    target_link_libraries(pspocket_platform_apple PUBLIC pspocket::core "-framework AVFoundation" "-framework AudioToolbox")
    target_compile_options(pspocket_platform_apple PRIVATE -fobjc-arc)
    set_target_properties(pspocket_platform_apple PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON
        OBJCXX_EXTENSIONS OFF)
    pspocket_set_warnings(pspocket_platform_apple)
    add_library(pspocket::platform_apple ALIAS pspocket_platform_apple)
endif()
