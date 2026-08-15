cmake_minimum_required(VERSION 3.24)

project(braam_hello LANGUAGES CXX)

# On its own this finds the installed SDK; inside Braam's own tree the targets
# are already defined and this file is one more add_subdirectory. One file both
# ways, so the example a user is given is one the build compiles.
if(NOT TARGET braam::proc)
    find_package(braam REQUIRED)
endif()

braam_add_program(NAME hello SOURCES hello.cpp)
