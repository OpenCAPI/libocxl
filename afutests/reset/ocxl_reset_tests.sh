#!/bin/bash
#
# Copyright 2019 International Business Machines
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# ocxl_reset_tests.sh
#
# This test assumes that user is root and memcpy afu is programmed.

function usage
{
	echo 'ocxl_reset_tests.sh [-d <device_path>] [-l <loops>]' >&2
	exit 2
}

device=
loops=1 # default

while true
do
	case $1 in
	('')	break ;;
	(-d)	device=$2; shift 2 || break ;;
	(-l)	loops=$2; shift 2 || break ;;
	(*)	usage ;;
	esac
done
(( $# == 0 )) || usage

[[ $device == -* ]] && usage
(( loops < 1 )) && usage

if [[ $device ]]
then
	if ! ls "$device" >/dev/null 2>&1
	then
		echo ocxl_reset_tests.sh: "$device": no such device >&2
		exit 2
	fi
<<<<<<< HEAD
	card=${device##*/}
=======
	card=${device##*/}	
>>>>>>> f2ad04f40dcceac9f4b2d7d6939e1963e6c2505d
fi

if [[ -z $card ]]
then
	# find first IBM,AFP3 or IBM,MEMCPY3 opencapi card
	card=$(
<<<<<<< HEAD
		set -- $(ls /dev/ocxl/ 2>/dev/null)
=======
		set -- $(ls /dev/ocxl/$card 2>/dev/null)
>>>>>>> f2ad04f40dcceac9f4b2d7d6939e1963e6c2505d
		for i
		do
			case $i in
			(*IBM,AFP3*) echo $i; break ;;
			(*IBM,MEMCPY3*) echo $i; break ;;
			esac
		done
	)
fi

if [[ -z $card ]]
then
	echo ocxl_reset_tests.sh: could not find afu IBM,AFP3 nor IBM,MEMCPY3 >&2
	exit 3
fi

# load module pnv-php
if ! modprobe pnv-php
then
	echo ocxl_reset_tests.sh: cannot load module pnv-php >&2
	exit 1
fi

<<<<<<< HEAD
slot=$(ls /dev/ocxl/$card | awk -F"." '{ print $2 }' | sed s/$/.0/)
slot=$(lspci -m -v -s $slot | awk '/^PhySlot:/ { print $2; exit }')
if [[ -z $slot ]]
then
	printf "$card: No slot found. Exiting.\n"
	exit 1
fi
slot=/sys/bus/pci/slots/$slot
=======
slot=${card#*.}
slot=${slot%%:*}
slot=/sys/bus/pci/slots/OPENCAPI-$slot
>>>>>>> f2ad04f40dcceac9f4b2d7d6939e1963e6c2505d

for ((i = 0; i < loops; i++))
do
	((loops > 1)) && echo Loop: $((i+1))/$loops

<<<<<<< HEAD
	echo ocxl_reset_tests.sh: resetting card $card in slot ${slot##*/}
=======
	echo ocxl_reset_tests.sh: resetting card $card
>>>>>>> f2ad04f40dcceac9f4b2d7d6939e1963e6c2505d
	if ! echo 0 > $slot/power
	then
		echo ocxl_reset_tests.sh: could not write to $slot/power
		exit 4
	fi

	if ! echo 1 > $slot/power
	then
		echo ocxl_reset_tests.sh: could not write to $slot/power
		exit 5
	fi

<<<<<<< HEAD
	echo ocxl_reset_tests.sh: card $card has been reset
=======
	echo ocxl_reset_tests.sh: card $card has been reset 
>>>>>>> f2ad04f40dcceac9f4b2d7d6939e1963e6c2505d

	case $card in
	(*,AFP3.*)
		ocxl_afp3=$(which ocxl_afp3 2>/dev/null)
		[[ $ocxl_afp3 ]] || ocxl_afp3=${0%/*}/ocxl_afp3

		if [[ ! -x $ocxl_afp3 ]]
		then
			echo ocxl_reset_tests.sh: could not find test program $ocxl_afp3
			echo ocxl_reset_tests.sh: skipping IBM,AFP3 afu check
		else
			echo ocxl_reset_tests.sh: verifying afu IBM,AFP3

			if ! "$ocxl_afp3" >/tmp/ocxl_reset_afp3.log
			then
				echo ocxl_reset_tests.sh: ocxl_afp3 fails after reset
				exit 6
			fi
		fi ;;
	(*,MEMCPY3.*)
		ocxl_memcpy=$(which ocxl_memcpy 2>/dev/null)
		[[ $ocxl_memcpy ]] || ocxl_memcpy=${0%/*}/ocxl_memcpy

		if [[ ! -x $ocxl_memcpy ]]
		then
			echo ocxl_reset_tests.sh: could not find test program $ocxl_memcpy
			echo ocxl_reset_tests.sh: skipping IBM,MEMCPY3 afu check
		else
			echo ocxl_reset_tests.sh: verifying afu IBM,MEMCPY3

			if ! "$ocxl_memcpy" -p0 -l10000 >/tmp/ocxl_reset_memcpy.log
			then
				echo ocxl_reset_tests.sh: ocxl_memcpy fails after reset
				exit 7
			fi
		fi ;;
	esac
done

echo ocxl_reset_tests.sh: ocxl_reset test passes
exit 0
