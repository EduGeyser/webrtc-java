#!/usr/bin/env bash
#
# Ends a hung test run before the job timeout does. A timeout cancels the job,
# which skips the failure-only upload of the reports, so a hang leaves nothing
# to look at. After the given minutes, every Gradle test JVM still running is
# taken as hung: its Java thread dump goes to the log and to hang-dumps/, its
# native stacks to hang-dumps/ where the runner has a tool for that (sample on
# macOS, gdb on Linux when installed), and then the JVM is ended, so the test
# task fails and the upload runs. Each dump has a deadline, so a JVM too stuck
# to answer is still ended.
#
# Usage: hang-watchdog.sh <minutes>, started in the background before the tests
# and stopped with SIGTERM after them.

set -u

DUMP_SECONDS=60

# Background children go with the watchdog: an orphaned one would keep the
# step's output open, and the step would wait for it.
trap 'kill $(jobs -p) 2> /dev/null; exit 0' TERM

# The PIDs of the running Gradle test JVMs. Gradle names them in their
# arguments; its other workers (compilers) are left alone.
test_jvms() {
	jps -lm | awk '/GradleWorkerMain/ && /Gradle Test Executor/ { print $1 }'
}

# Runs a command, and kills it once the given seconds have passed.
bounded() {
	local seconds="$1"
	shift
	"$@" &
	local command=$!
	( sleep "$seconds"; kill -9 "$command" ) > /dev/null 2>&1 &
	local deadline=$!
	wait "$command" 2> /dev/null
	kill "$deadline" 2> /dev/null
}

sleep $(( $1 * 60 )) &
wait $!

pids=$(test_jvms)

if [ -z "$pids" ]; then
	exit 0
fi

mkdir -p hang-dumps
echo "::error::Tests still running after $1 minutes; dumping and ending the test JVMs $(echo $pids)"

for pid in $pids; do
	bounded "$DUMP_SECONDS" jcmd "$pid" Thread.print -l > "hang-dumps/java-threads-$pid.txt" 2>&1

	case "${RUNNER_OS:-}" in
		macOS)
			bounded "$DUMP_SECONDS" sample "$pid" 5 -file "hang-dumps/native-stacks-$pid.txt" > /dev/null 2>&1
			;;
		Linux)
			if command -v gdb > /dev/null; then
				bounded "$DUMP_SECONDS" sudo -n gdb -batch -p "$pid" -ex "thread apply all bt" \
					> "hang-dumps/native-stacks-$pid.txt" 2>&1
			fi
			;;
	esac

	echo "::group::Java threads of test JVM $pid"
	cat "hang-dumps/java-threads-$pid.txt"
	echo "::endgroup::"
done

# Only what is still a test JVM: a PID could have been reused while dumping.
still=$(test_jvms)

for pid in $pids; do
	case " $(echo $still) " in
		*" $pid "*)
			if [ "${RUNNER_OS:-}" = "Windows" ]; then
				taskkill //F //PID "$pid" > /dev/null
			else
				kill -9 "$pid"
			fi
			;;
	esac
done
