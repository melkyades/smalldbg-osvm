# OpenSmalltalk (Spur): its VM layer and its Webside server, built on
# smalldbg's vmdbg and webside. Serves Cuis and Squeak-family images.

add_library(vmdbg_osvm STATIC
    vmdbg/Smalltalk/OSVM/CogFrame.cpp
    vmdbg/Smalltalk/OSVM/CogStackZone.cpp
    vmdbg/Smalltalk/OSVM/OSVMInspector.cpp
    vmdbg/Smalltalk/OSVM/SpurHeapObject.cpp
    vmdbg/Smalltalk/OSVM/SpurImage.cpp
    vmdbg/Smalltalk/OSVM/SpurImageFile.cpp
    vmdbg/Smalltalk/OSVM/SpurKnownObjects.cpp
    vmdbg/Smalltalk/OSVM/SistaV1Disassembler.cpp
    vmdbg/Smalltalk/OSVM/SpurActivation.cpp
    vmdbg/Smalltalk/OSVM/SpurProcessMemory.cpp
    vmdbg/Smalltalk/OSVM/SpurSourceFiles.cpp
)
target_include_directories(vmdbg_osvm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vmdbg)
target_link_libraries(vmdbg_osvm PUBLIC vmdbg)

add_executable(webside_osvm
    webside/main.cpp
    webside/OSVMWebsideServer.cpp
    webside/OSVMDebugSession.cpp
    webside/OSVMInspectorJson.cpp
)
target_link_libraries(webside_osvm PRIVATE vmdbg_osvm webside_common)
target_compile_features(webside_osvm PRIVATE cxx_std_17)

# Reads an image file through the Spur object model, with no VM and no
# debugger: osvm_image_test <path/to/Cuis.image>
add_executable(osvm_image_test vmdbg/Smalltalk/OSVM/osvm_image_test.cpp)
target_link_libraries(osvm_image_test PRIVATE vmdbg_osvm)

# Run with the other tests when OSVM_TEST_IMAGE names a 64-bit Spur image; a
# Cuis one is what the checks were written against.
set(OSVM_TEST_IMAGE "" CACHE FILEPATH "A 64-bit Spur image for osvm_image_test")
if(OSVM_TEST_IMAGE)
    add_test(NAME osvm-image-test COMMAND osvm_image_test ${OSVM_TEST_IMAGE})
endif()

if(APPLE)
    set(ENTITLEMENTS_FILE "${CMAKE_SOURCE_DIR}/debugger.entitlements")
    add_custom_command(TARGET webside_osvm POST_BUILD
        COMMAND codesign --force --sign - --entitlements ${ENTITLEMENTS_FILE} $<TARGET_FILE:webside_osvm>
        COMMENT "Signing webside_osvm with debugger entitlement"
    )
endif()
