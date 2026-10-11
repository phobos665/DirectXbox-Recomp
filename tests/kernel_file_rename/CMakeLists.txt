# kernel_file_rename -- NtSetInformationFile(FileRenameInformation) on the
# host file backend (xbox_RenameFileByHandle in src/kernel/kernel_file.c):
# path-table and RootDirectory resolution, ReplaceIfExists, bare names, and a
# handle that follows its file. Portable (Win32 and POSIX backends). No game
# files: a temporary game folder and save folder, removed afterwards.

add_executable(kernel_file_rename_test test_main.c)
target_include_directories(kernel_file_rename_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel)
target_link_libraries(kernel_file_rename_test PRIVATE xboxrecomp)

add_test(NAME kernel_file_rename COMMAND kernel_file_rename_test)
