cmake_minimum_required(VERSION 3.20)
project(CreativeSystemAtlas VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4        QUIET IMPORTED_TARGET gtk4)
pkg_check_modules(EGL         QUIET IMPORTED_TARGET egl)
pkg_check_modules(WAYLAND_EGL QUIET IMPORTED_TARGET wayland-egl)

# ── Cœur moteur (sans dépendance à la boîte à outils UI) ──────────────────────
add_library(CreativeCore STATIC
    engine/memory/memory_manager.cpp
    engine/fusion/fusion_creator_engine.cpp
    engine/tiles/tile_store.cpp
    engine/brush/brush_engine.cpp
    engine/layers/layer_stack.cpp
    engine/tools/tool_system.cpp
    engine/serialization/atlas_project.cpp
    engine/layers/layer_compositor.cpp
    engine/brush/bitmap_brush.cpp
    engine/brush/advanced_brushes.cpp
)
target_include_directories(CreativeCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(PNG  REQUIRED)
find_package(ZLIB REQUIRED)
find_package(TIFF REQUIRED)
target_link_libraries(CreativeCore PUBLIC PNG::PNG ZLIB::ZLIB TIFF::TIFF)

pkg_check_modules(PYTHON_EMBED REQUIRED IMPORTED_TARGET python3-embed)
target_link_libraries(CreativeCore PUBLIC PkgConfig::PYTHON_EMBED)

find_library(GLES2_LIBRARY GLESv2)
if(GLES2_LIBRARY)
    target_link_libraries(CreativeCore PUBLIC ${GLES2_LIBRARY})
endif()

# EGL est lié en PUBLIC : les applications en ont besoin, et sur certaines
# distributions libGLESv2 exige libEGL sur la ligne de link (« DSO missing »).
# Le moteur lui-même n'appelle plus EGL depuis que LayerCompositor est CPU pur ;
# il n'utilise GL que pour valider les shaders de fusion (fusion_creator_engine).
# Repli sur find_library quand la distribution ne fournit pas egl.pc.
find_library(EGL_LIB_PATH EGL)
if(TARGET PkgConfig::EGL)
    target_link_libraries(CreativeCore PUBLIC PkgConfig::EGL)
elseif(EGL_LIB_PATH)
    target_link_libraries(CreativeCore PUBLIC ${EGL_LIB_PATH})
else()
    message(WARNING "EGL introuvable : le link risque d'échouer.")
endif()

# ── Applications GTK4 ─────────────────────────────────────────────────────────
if(NOT GTK4_FOUND)
    message(STATUS "GTK4 absent : aucune application ne sera construite.")
else()
    find_package(OpenGL REQUIRED)

    # Surface GPU, renderer et widgets GTK réutilisables. Ils vivent
    # dans la couche UI, pas dans CreativeCore, pour que le moteur reste
    # indépendant de GTK et de Wayland.
    set(ATLAS_UI_SOURCES
        engine/render/native_renderer.cpp
        ui/gtk/native_canvas_surface.cpp
        ui/gtk/color_wheel.cpp
    )

    # Applique à une cible la configuration UI commune.
    function(atlas_configure_app target)
        target_link_libraries(${target}
            PRIVATE CreativeCore PkgConfig::GTK4 OpenGL::GL dl)
        if(TARGET PkgConfig::WAYLAND_EGL)
            target_link_libraries(${target} PRIVATE PkgConfig::WAYLAND_EGL)
        else()
            find_library(WAYLAND_EGL_LIB_PATH wayland-egl)
            if(WAYLAND_EGL_LIB_PATH)
                target_link_libraries(${target} PRIVATE ${WAYLAND_EGL_LIB_PATH})
            endif()
        endif()
        target_compile_definitions(${target}
            PRIVATE CREATIVE_WAYLAND_NATIVE=1 CREATIVE_GTK4=1)
    endfunction()

    # app/main.cpp n'est plus construit : c'est une ancienne copie de l'UI
    # Atlas (même titre, même code), sans --self-test, restée figée pendant
    # que ui/gtk/main.cpp évoluait. Le fichier est conservé sur le disque.

    # Atlas.
    add_executable(CreativeSystemAtlasGtk ui/gtk/main.cpp ${ATLAS_UI_SOURCES})
    atlas_configure_app(CreativeSystemAtlasGtk)
endif()

# ── Installation ──────────────────────────────────────────────────────────────
if(TARGET CreativeSystemAtlasGtk)
    install(TARGETS CreativeSystemAtlasGtk RUNTIME DESTINATION bin)
endif()
install(DIRECTORY api engine DESTINATION include/CreativeSystem)
