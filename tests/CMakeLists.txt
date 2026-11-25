add_test(NAME asm816_version
    COMMAND asm816 --version)
set_tests_properties(asm816_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^asm816 0\\.0\\.1")
