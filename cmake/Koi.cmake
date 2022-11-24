OPTION(BUILD_KOI_FROM_SOURCE OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)

    if(BUILD_KOI_FROM_SOURCE)
        message(STATUS "Building Koi from source")

        set(KOI_DIR "${DORADO_3RD_PARTY}/koi")

        if(NOT EXISTS ${KOI_DIR})
            if(DEFINED GITLAB_CI_TOKEN)
                message("Cloning Koi using CI token")
                execute_process(COMMAND git clone https://gitlab-ci-token:${GITLAB_CI_TOKEN}@git.oxfordnanolabs.local/machine-learning/koi.git ${KOI_DIR})
            else()
                message("Cloning Koi using ssh")
                execute_process(COMMAND git clone git@git.oxfordnanolabs.local:machine-learning/koi.git ${KOI_DIR})
            endif()
            execute_process(COMMAND git checkout 0174a5c20a6c5bd18dc2e61b39a25d297da6a814 WORKING_DIRECTORY ${KOI_DIR})
            execute_process(COMMAND git submodule update --init --checkout WORKING_DIRECTORY ${KOI_DIR})
        endif()
        add_subdirectory(${KOI_DIR}/koi/lib)

        set(KOI_INCLUDE ${KOI_DIR}/koi/lib)
        set(KOI_LIBRARIES koi)
    else()
        message(STATUS "Using prebuilt Koi from Box")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            download_and_extract(https://nanoporetech.box.com/shared/static/tphtmha3qoyd6j2amyr40qq0ebed4b4d.gz koi_lib)
            file(GLOB KOI_DIR "${DORADO_3RD_PARTY}/koi_lib/*")
            set(KOI_LIBRARIES ${KOI_DIR}/lib/libkoi.a)
        elseif(WIN32)
            download_and_extract(https://nanoporetech.box.com/shared/static/fnqaj8v48kwvvmvdmf8ici6qh3mp6ogu.zip koi_lib)
            file(GLOB KOI_DIR "${DORADO_3RD_PARTY}/koi_lib/*")
            set(KOI_LIBRARIES ${KOI_DIR}/lib/koi.lib)
        endif()
        message(STATUS "KOI_DIR is ${KOI_DIR}")
        set(KOI_INCLUDE ${KOI_DIR}/include)
        message(STATUS "KOI_INCLUDE is ${KOI_INCLUDE}")
        execute_process(COMMAND ls ${KOI_INCLUDE})
    endif()
endif()
