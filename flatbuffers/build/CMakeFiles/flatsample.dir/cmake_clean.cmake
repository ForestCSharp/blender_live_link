file(REMOVE_RECURSE
  "samples/monster_generated.h"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/flatsample.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
