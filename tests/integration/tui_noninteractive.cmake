# 无终端环境下 TUI 必须快速失败，不能进入事件循环等待输入。
execute_process(
  COMMAND "${SLIMESEEKER_CLI}" --tui
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(result EQUAL 0)
  message(FATAL_ERROR "TUI unexpectedly succeeded without an interactive terminal")
endif()
string(CONCAT message "${output}" "${error}")
if(NOT message MATCHES "interactive terminal|required|交互式终端")
  message(FATAL_ERROR "TUI did not report the terminal requirement: ${message}")
endif()
