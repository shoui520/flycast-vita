target_sources(${PROJECT_NAME} PRIVATE
        audiostream.h)

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
            audiobackend_alsa.cpp
            audiobackend_libao.cpp
            audiobackend_null.cpp
            audiobackend_oss.cpp
            audiobackend_pulseaudio.cpp
            audiobackend_sdl2.cpp
            audiostream.cpp)
endif()
