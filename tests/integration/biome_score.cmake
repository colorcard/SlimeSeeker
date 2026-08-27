execute_process(
  COMMAND "${SLIMESEEKER_CLI}" biome-score --threads 1 --backend scalar --quiet
          --format csv --top 2 0 64 20
  RESULT_VARIABLE status
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "biome-score failed (${status}): ${error}")
endif()

set(expected
"rank,x,z,count,biome_score,common_equivalent_chunks,player_x,player_y,player_z,afk_score
1,14,19,37,28.649557977,36.886305895,233.5,-38,319.5,27.755076368
2,14,20,37,28.649557977,36.886305895,230.5,-38,324.5,28.503137580
")
if(NOT output STREQUAL expected)
  message(FATAL_ERROR "unexpected biome-score output:\n${output}")
endif()
