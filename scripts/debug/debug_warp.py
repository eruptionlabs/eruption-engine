import subprocess
import time
import sys
import os
import signal
import re

def run_test():
    print("=== Eruption Engine Warp Debugger ===")
    print("Redirection: All engine output is being sent to 'warp_debug_output.txt'")
    
    # Configuration
    loading_timeout = 20.0
    warp_timeout = 15.0
    post_warp_wait = 2.0
    
    # Command to run
    cmd = ["./eruption-engine", "--auto-exit", "--auto-test"]
    
    # Open log file
    log_file = open("warp_debug_output.txt", "w", encoding='utf-8', errors='replace')
    
    process = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT, text=True, bufsize=1)

    start_time = time.time()
    last_heartbeat = start_time
    warp_initiated = False
    warp_initiated_time = 0
    swap_completed = False
    initial_load_done = False
    
    # We still need to poll the log file for state tracking, but without printing millions of lines
    log_read_handle = open("warp_debug_output.txt", "r", encoding='utf-8', errors='replace')

    try:
        while process.poll() is None:
            # Read new lines from log file
            line = log_read_handle.readline()
            if not line:
                time.sleep(0.1)
                # Still check timeouts even if no new lines
                now = time.time()
                if warp_initiated and not swap_completed and (now - warp_initiated_time) > warp_timeout:
                    print("\n!!! ERROR: Warp sequence timed out.")
                    break
                continue

            # State tracking (silent)
            if "Engine: map swap complete" in line or "Engine: swap complete" in line:
                if not initial_load_done:
                    initial_load_done = True
                    print(f">> [DEBUG] Initial Map Load Complete.")
                else:
                    swap_completed = True
                    print(f">> [DEBUG] Warp Swap Complete.")

            if "Initiating background warp" in line:
                warp_initiated = True
                warp_initiated_time = time.time()
                print(">> [DEBUG] Warp Sequence Initiated.")

            if "DEBUG HEARTBEAT" in line:
                last_heartbeat = time.time()

            # Timeouts
            now = time.time()
            if now - last_heartbeat > 10.0 and initial_load_done:
                print("\n!!! ERROR: HEARTBEAT LOST!")
                break

            # Success condition
            if swap_completed and (now - last_heartbeat) > post_warp_wait:
                print("\n=== SUCCESS: Warp verified! ===")
                break

    except KeyboardInterrupt:
        print("\nTest interrupted by user.")
    finally:
        process.terminate()
        log_file.close()
        log_read_handle.close()

    return swap_completed

if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)
