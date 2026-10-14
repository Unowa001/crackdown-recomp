"""Host a local Archipelago test server and forward console commands.

Runs the installed ArchipelagoServer.exe on 127.0.0.1 for a generated seed.
Lines appended to COMMANDS are sent to the server console (e.g.
"/send_location Partner Agility Orb 176"); the console output goes to LOG.
Usage: ap_server.py SEED.zip LOG COMMANDS [--port 38281]
"""
import argparse
import subprocess
import time

SERVER = r"C:\ProgramData\Archipelago\ArchipelagoServer.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seed")
    ap.add_argument("log")
    ap.add_argument("commands")
    ap.add_argument("--port", type=int, default=38281)
    args = ap.parse_args()
    open(args.commands, "a").close()
    with open(args.log, "ab") as log:
        server = subprocess.Popen([SERVER, "--host", "127.0.0.1", "--port", str(args.port),
                                   "--loglevel", "debug", args.seed],
                                  stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                                  cwd=r"C:\ProgramData\Archipelago")
        done = 0
        try:
            while server.poll() is None:
                with open(args.commands, encoding="utf-8") as f:
                    lines = f.read().splitlines()
                for line in lines[done:]:
                    if line.strip() == "!quit":
                        server.terminate()
                        break
                    server.stdin.write((line + "\n").encode())
                    server.stdin.flush()
                done = len(lines)
                time.sleep(0.25)
        finally:
            if server.poll() is None:
                server.terminate()


if __name__ == "__main__":
    main()
