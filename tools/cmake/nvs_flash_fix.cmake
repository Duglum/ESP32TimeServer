# ESP32-WebTherm
# Copyright Duglum, 2026. License: MIT (see LICENSE)
#
# ESP-IDF's nvs_flash component with two corrections, built from a copy in the build folder.
#
# ESP-IDF 6.1.1, components/nvs_flash/src/nvs_page.cpp, Page::mLoadEntryTable() (called by Page::load() for each
# page), the branch of the ACTIVE page. At every start the NVS code checks the active page for items that were written
# twice (a new version written, the start came before the old one was erased) and erases the earlier one. Both checks
# can erase an item that is no duplicate:
#
# 1. "check that last item is not duplicate": findItem() is called for the last item of the page without its chunk
#    index. For a data chunk of a blob it then finds any data chunk of the same key on that page, whatever its chunk
#    index and version, and erases it. Storage::init() then erases the index of that blob, whose chunks no longer
#    match, and every chunk left of it: the whole blob, the stored version and the new one, is gone. The page has
#    that layout after a reset between the data chunk and the index of a blob update, and after a garbage
#    collection that leaves two chunks of one blob on the active page with a data chunk last. Correction: the chunk
#    index of the last item is passed (for every item but a blob data chunk it is CHUNK_ANY, as before).
# 2. In the loop over the entries, an earlier item whose 24-bit hash (namespace index, key, chunk index) equals that
#    of a later item is erased without comparing namespace index, key and chunk index: two different items with the
#    same hash lose the earlier one. Correction: the earlier item is erased only when those three are equal; when
#    they are not, the next earlier item with that hash is checked. The data type is still not compared (an old
#    format blob is replaced by a blob index of the same key, as ESP-IDF's comment above that line says).
#
# Include this file in the project's CMakeLists.txt before project(). It copies CMakeLists.txt, Kconfig,
# project_include.cmake, src, include and private_include of $IDF_PATH/components/nvs_flash into
# <build folder>/nvs_flash_fix/nvs_flash, corrects the two places in the copy of src/nvs_page.cpp and adds the copy
# to EXTRA_COMPONENT_DIRS: a component of EXTRA_COMPONENT_DIRS takes the place of ESP-IDF's component of the same
# name. Every file of the copy is ESP-IDF's file byte for byte, apart from the two replaced texts. The bootloader is
# built with its own component list and gets ESP-IDF's nvs_flash, which it builds without Page::mLoadEntryTable().
# ESP-IDF itself is not changed.
#
# The path of the copy in the image. ESP-IDF replaces the project folder and IDF_PATH in __FILE__ (assert texts) with
# -fmacro-prefix-map=<project>=. and <IDF_PATH>=/IDF (CONFIG_COMPILER_HIDE_PATHS_MACROS), but the copy lies in the
# build folder: without a map of its own an assert text of the copy holds the build folder (./<build>/nvs_flash_fix/...
# inside the project, the absolute path outside it). So the script adds -fmacro-prefix-map=<copy>=<name of ESP-IDF's
# component> to ESP-IDF's COMPILE_OPTIONS (every component, as ESP-IDF's own maps): the copy's files read as ESP-IDF's
# own, /IDF/components/nvs_flash/... (or <IDF_PATH>/components/nvs_flash/... when that option is off). Of several
# matching -fmacro-prefix-map options GCC uses the last one given (the manual says nothing about it; measured with
# GCC 15.2 of ESP-IDF 6.1.1), and inside the project <project>=. matches the copy too: the option is added by a
# deferred call at the end of the CMakeLists.txt that includes this file, after project() has added ESP-IDF's maps.
# The line numbers in an assert text are those of the corrected file. Debug information (ELF only) is not mapped:
# without CONFIG_APP_REPRODUCIBLE_BUILD ESP-IDF's own components carry the build folder there too (DW_AT_comp_dir),
# with it ESP-IDF maps every component folder, the copy's included.
#
# Three cases, decided at every CMake configure:
# - both faulty texts are there exactly once: the copy is made (only changed files are written) and used;
# - both corrected texts are there exactly once (an ESP-IDF with these corrections): ESP-IDF's own component is used,
#   no copy;
# - anything else: the configure step stops. This script then has to be looked at for that ESP-IDF version.
# A change of ESP-IDF's files or of this script runs the configure step again (CMAKE_CONFIGURE_DEPENDS).
#
# This file can go when the ESP-IDF version in use has both corrections (or corrects both places otherwise; the
# configure step then stops with the third case). Upstream on 2026-11-20: reported as espressif/esp-idf issues #19212
# (place 1; pull request #19376, the same change, open) and #19213 (place 2), both open; master and release/v6.1 still
# had both faulty texts (master's call of place 1 has two more arguments, so this script stops there). It is the same file, unchanged, in other projects that use it;
# nothing in it is specific to this project.

set(nvs_fix_source "$ENV{IDF_PATH}/components/nvs_flash")
set(nvs_fix_copy "${CMAKE_BINARY_DIR}/nvs_flash_fix/nvs_flash")
set(nvs_fix_page "${nvs_fix_source}/src/nvs_page.cpp")
set(nvs_fix_name "nvs_flash_fix.cmake")

if(NOT EXISTS "${nvs_fix_page}")
    message(FATAL_ERROR "${nvs_fix_name}: ${nvs_fix_page} not found (IDF_PATH is '$ENV{IDF_PATH}')")
endif()

# CMake reads a text file with its line ends turned into LF. The copy has to keep the line ends of ESP-IDF's file:
# find out which they are and refuse a file whose bytes are neither pure LF nor pure CRLF.
file(READ "${nvs_fix_page}" nvs_fix_text)
file(SHA256 "${nvs_fix_page}" nvs_fix_source_hash)
string(SHA256 nvs_fix_lf_hash "${nvs_fix_text}")
string(REPLACE "\n" "\r\n" nvs_fix_text_crlf "${nvs_fix_text}")
string(SHA256 nvs_fix_crlf_hash "${nvs_fix_text_crlf}")
if(nvs_fix_source_hash STREQUAL nvs_fix_lf_hash)
    set(nvs_fix_newline_style UNIX)
elseif(nvs_fix_source_hash STREQUAL nvs_fix_crlf_hash)
    set(nvs_fix_newline_style DOS)
else()
    message(FATAL_ERROR "${nvs_fix_name}: ${nvs_fix_page} has mixed line ends or bytes CMake cannot read back "
                        "exactly; the script has to be looked at for this ESP-IDF version")
endif()

# Place 1: the call in the block "check that last item is not duplicate".
set(nvs_fix_faulty_1 "findItem(item.nsIndex, item.datatype, item.key, findItemIndex, dupItem) == ESP_OK")
set(nvs_fix_corrected_1 "findItem(item.nsIndex, item.datatype, item.key, findItemIndex, dupItem, item.chunkIndex) == ESP_OK")
# Place 2: the erase by hash in the loop over the entries.
string(CONCAT nvs_fix_faulty_2
    "            if (duplicateIndex < i) {\n"
    "                eraseEntryAndSpan(duplicateIndex, DEFAULT_PURGE_AFTER_ERASE);\n"
    "            }\n")
string(CONCAT nvs_fix_corrected_2
    "            while (duplicateIndex < i) {\n"
    "                Item duplicateItem;\n"
    "                err = readEntry(duplicateIndex, duplicateItem);\n"
    "                if (err != ESP_OK) {\n"
    "                    mState = PageState::INVALID;\n"
    "                    return err;\n"
    "                }\n"
    "                if (duplicateItem.nsIndex == item.nsIndex && duplicateItem.chunkIndex == item.chunkIndex\n"
    "                        && strncmp(duplicateItem.key, item.key, Item::MAX_KEY_LENGTH) == 0) {\n"
    "                    eraseEntryAndSpan(duplicateIndex, DEFAULT_PURGE_AFTER_ERASE);\n"
    "                    break;\n"
    "                }\n"
    "                duplicateIndex = mHashList.find(duplicateIndex + 1, item);\n"
    "            }\n")

# Number of times <needle> is in nvs_fix_text (0, 1, or 2 for "more than once").
function(nvs_fix_count needle out)
    string(FIND "${nvs_fix_text}" "${needle}" first)
    string(FIND "${nvs_fix_text}" "${needle}" last REVERSE)
    if(first EQUAL -1)
        set(${out} 0 PARENT_SCOPE)
    elseif(first EQUAL last)
        set(${out} 1 PARENT_SCOPE)
    else()
        set(${out} 2 PARENT_SCOPE)
    endif()
endfunction()

# Deferred to the end of the including CMakeLists.txt: __FILE__ of the copy <copy> reads as ESP-IDF's nvs_flash (see
# the header). The sdkconfig values are read here, in the function's own scope.
function(nvs_fix_map_copy copy)
    if(NOT COMMAND idf_build_set_property)
        message(FATAL_ERROR "nvs_flash_fix.cmake: ESP-IDF's project.cmake was not included in the CMakeLists.txt that "
                            "includes this file")
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    idf_build_get_property(sdkconfig_cmake SDKCONFIG_CMAKE)
    include("${sdkconfig_cmake}")
    if(CONFIG_COMPILER_HIDE_PATHS_MACROS)
        set(name "/IDF/components/nvs_flash")
    else()
        set(name "${idf_path}/components/nvs_flash")
    endif()
    idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=${copy}=${name}" APPEND)
endfunction()

nvs_fix_count("${nvs_fix_faulty_1}" nvs_fix_f1)
nvs_fix_count("${nvs_fix_corrected_1}" nvs_fix_c1)
nvs_fix_count("${nvs_fix_faulty_2}" nvs_fix_f2)
nvs_fix_count("${nvs_fix_corrected_2}" nvs_fix_c2)

# ESP-IDF's file is a configure input in every case: a new ESP-IDF version runs this script again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nvs_fix_page}")

if(nvs_fix_f1 EQUAL 1 AND nvs_fix_c1 EQUAL 0 AND nvs_fix_f2 EQUAL 1 AND nvs_fix_c2 EQUAL 0)
    string(REPLACE "${nvs_fix_faulty_1}" "${nvs_fix_corrected_1}" nvs_fix_fixed "${nvs_fix_text}")
    string(REPLACE "${nvs_fix_faulty_2}" "${nvs_fix_corrected_2}" nvs_fix_fixed "${nvs_fix_fixed}")

    # Every file of the parts the build uses, byte for byte; nvs_page.cpp is written below. A file is only written
    # when it differs (no recompile at an unchanged configure); a file that ESP-IDF no longer has is removed.
    set(nvs_fix_parts CMakeLists.txt Kconfig project_include.cmake src include private_include)
    set(nvs_fix_files "")
    foreach(part ${nvs_fix_parts})
        if(IS_DIRECTORY "${nvs_fix_source}/${part}")
            file(GLOB_RECURSE part_files LIST_DIRECTORIES false RELATIVE "${nvs_fix_source}" "${nvs_fix_source}/${part}/*")
            list(APPEND nvs_fix_files ${part_files})
        elseif(EXISTS "${nvs_fix_source}/${part}")
            list(APPEND nvs_fix_files "${part}")
        else()
            message(FATAL_ERROR "${nvs_fix_name}: ${nvs_fix_source}/${part} not found; the script has to be looked at "
                                "for this ESP-IDF version")
        endif()
    endforeach()
    if(EXISTS "${nvs_fix_copy}")
        file(GLOB_RECURSE nvs_fix_old_files LIST_DIRECTORIES false RELATIVE "${nvs_fix_copy}" "${nvs_fix_copy}/*")
        foreach(file ${nvs_fix_old_files})
            if(NOT file IN_LIST nvs_fix_files)
                file(REMOVE "${nvs_fix_copy}/${file}")
            endif()
        endforeach()
    endif()
    foreach(file ${nvs_fix_files})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nvs_fix_source}/${file}")
        if(NOT file STREQUAL "src/nvs_page.cpp")
            get_filename_component(file_dir "${nvs_fix_copy}/${file}" DIRECTORY)
            file(MAKE_DIRECTORY "${file_dir}")
            file(COPY_FILE "${nvs_fix_source}/${file}" "${nvs_fix_copy}/${file}" ONLY_IF_DIFFERENT)
        endif()
    endforeach()

    # file(WRITE) would write CRLF on Windows; file(CONFIGURE) writes the given line ends and only when the content
    # changed. It replaces @name@: every @ is written as @nvs_fix_at@, which it turns back into @. The result is
    # checked against the expected bytes.
    set(nvs_fix_at "@")
    string(REPLACE "@" "@nvs_fix_at@" nvs_fix_escaped "${nvs_fix_fixed}")
    file(CONFIGURE OUTPUT "${nvs_fix_copy}/src/nvs_page.cpp" CONTENT "${nvs_fix_escaped}" @ONLY
         NEWLINE_STYLE ${nvs_fix_newline_style})
    if(nvs_fix_newline_style STREQUAL "DOS")
        string(REPLACE "\n" "\r\n" nvs_fix_fixed "${nvs_fix_fixed}")
    endif()
    string(SHA256 nvs_fix_expected_hash "${nvs_fix_fixed}")
    file(SHA256 "${nvs_fix_copy}/src/nvs_page.cpp" nvs_fix_written_hash)
    if(NOT nvs_fix_written_hash STREQUAL nvs_fix_expected_hash)
        file(REMOVE "${nvs_fix_copy}/src/nvs_page.cpp")
        message(FATAL_ERROR "${nvs_fix_name}: the corrected copy of nvs_page.cpp is not the expected text; the script "
                            "has to be looked at")
    endif()

    list(APPEND EXTRA_COMPONENT_DIRS "${nvs_fix_copy}")
    # The copy's path in __FILE__ (see the header): at the end of the including CMakeLists.txt, after project(). The
    # bracket argument keeps the path as it is now.
    cmake_language(EVAL CODE "cmake_language(DEFER CALL nvs_fix_map_copy [==[${nvs_fix_copy}]==])")
    message(STATUS "${nvs_fix_name}: nvs_flash is built from ${nvs_fix_copy} (Page::mLoadEntryTable() of nvs_page.cpp "
                   "corrected in two places)")
elseif(nvs_fix_f1 EQUAL 0 AND nvs_fix_c1 EQUAL 1 AND nvs_fix_f2 EQUAL 0 AND nvs_fix_c2 EQUAL 1)
    file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/nvs_flash_fix")
    message(STATUS "${nvs_fix_name}: ESP-IDF's nvs_flash has both corrections already; it is used as it is")
else()
    file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/nvs_flash_fix")
    message(FATAL_ERROR "${nvs_fix_name}: ${nvs_fix_page} does not have the expected text of "
                        "Page::mLoadEntryTable() (faulty text 1/2 found ${nvs_fix_f1}/${nvs_fix_f2} times, corrected "
                        "text 1/2 found ${nvs_fix_c1}/${nvs_fix_c2} times; 2 means more than once). The script has to "
                        "be looked at for this ESP-IDF version.")
endif()
