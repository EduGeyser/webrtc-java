#!/usr/bin/env bash
#
# Ends a hung test run before the job timeout does. A timeout cancels the job,
# which skips the failure-only upload of the reports, so a hang leaves nothing
# to look at. After the given minutes, every Gradle test JVM still running is
# taken as hung: its Java thread dump goes to the log and to hang-dumps/, its
# native stacks to hang-dumps/ where the runner has a tool for that (sample on
# macOS, gdb on Linux when installed), and then the JVM is ended, so the test
# task fails and the upload runs.
#
# Usage: hang-watchdog.sh <minutes>, started in the background before the tests
# and stopped with SIGTERM after them.

set -u

# The sleep runs in the background and goes with the watchdog: an orphaned
# sleep would keep the step's output open, and the step would wait for it.
sleep $(( $1 * 60 )) &
sleeper=$!
trap 'kill "$sleeper" 2> /dev/null; exit 0' TERM
wait "$sleeper"
trap - TERM

pids=$(jps -l | awk '/GradleWorkerMain/ { print $1 }')

if [ -z "$pids" ]; then
	exit 0
fi

mkdir -p hang-dumps
echo "::error::Tests still running after $1 minutes; dumping and ending the test JVMs $(echo $pids)"

for pid in $pids; do
	jcmd "$pid" Thread.print -l > "hang-dumps/java-threads-$pid.txt" 2>&1

	case "${RUNNER_OS:-}" in
		macOS)
			sample "$pid" 5 -file "hang-dumps/native-stacks-$pid.txt" > /dev/null 2>&1
			;;
		Linux)
			if command -v gdb > /dev/null; then
				sudo -n gdb -batch -p "$pid" -ex "thread apply all bt" > "hang-dumps/native-stacks-$pid.txt" 2>&1
			fi
			;;
	esac

	echo "::group::Java threads of test JVM $pid"
	cat "hang-dumps/java-threads-$pid.txt"
	echo "::endgroup::"
done

for pid in $pids; do
	if [ "${RUNNER_OS:-}" = "Windows" ]; then
		taskkill //F //PID "$pid" > /dev/null
	else
		kill -9 "$pid"
	fi
done
