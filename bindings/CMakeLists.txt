if(DSPACES_HAVE_FORTRAN)
    add_subdirectory(fortran)
endif()

if(DSPACES_BUILD_PYTHON_BINDINGS)
    add_subdirectory(python)
endif()
