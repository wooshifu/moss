# src/modules/CMakeLists.txt

# Create moss.std module
moss_add_module(moss_std
    SOURCES std.cppm
)

# Set module properties
set_target_properties(moss_std PROPERTIES
    EXPORT_NAME "moss.std"
)

# Add moss.types module
moss_add_module(moss_types
    SOURCES types.cppm
    DEPENDENCIES moss_std
)

set_target_properties(moss_types PROPERTIES
    EXPORT_NAME "moss.types"
)