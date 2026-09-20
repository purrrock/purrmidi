C:/msys64/ucrt64/bin/openocd.exe -c "gdb_port 50000" -c "tcl_port 50001" -c "telnet_port 50002" -s "C:/msys64/ucrt64/share/openocd/scripts" -f interface/stlink-dap.cfg -f target/stm32f4x.cfg
pause