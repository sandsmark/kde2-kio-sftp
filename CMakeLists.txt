cmake_minimum_required(VERSION 3.1.0)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
project(kde2-kio-sftp)

find_package(Qt2 REQUIRED)

find_package(ZLIB REQUIRED)
find_package(kdelibs2 REQUIRED)
find_package(DCOP REQUIRED)
find_package(kdecore REQUIRED)
find_package(kdeui REQUIRED)
find_package(kssl REQUIRED)
find_package(kdesu REQUIRED)
find_package(kio REQUIRED)
find_package(ksycoca REQUIRED)

set(kio_sftp_SRC
process.cpp atomicio.cpp kio_sftp.cpp sftpfileattr.cpp ksshprocess.cpp
    )
#qt2_wrap_moc(kio_sftp_SRC
#    SOURCES
#    kio_sftp.h
#    )
kde2_module(kio_sftp SOURCES ${kio_sftp_SRC} LIBS kde2::kio kde2::ksycoca)
install(FILES sftp.protocol DESTINATION "${KDE2_SERVICESDIR}")

include(CheckIncludeFiles)
check_include_files(sys/select.h HAVE_SYS_SELECT_H)
if(HAVE_SYS_SELECT_H)
    target_compile_definitions(module_kio_sftp PRIVATE HAVE_SYS_SELECT_H)
endif()
check_include_files(sys/time.h HAVE_SYS_TIME_H)
if(HAVE_SYS_TIME_H)
    target_compile_definitions(module_kio_sftp PRIVATE HAVE_SYS_TIME_H)
endif()
