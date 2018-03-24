#!/bin/sh

#set -x
set -e

if [ $# -ne 1 ]; then
	echo "Incorrect number of parameters."
	echo "Usage: $(basename "$0") [Maschine Jam Sysfs Path]"
	echo "Exiting now."
	exit 1
fi

MASCHINE_JAM_SYSFS_DIR="$(readlink -m "$1")"
if [ ! -d "$MASCHINE_JAM_SYSFS_DIR" ]; then
	echo "Error: directory [Maschine Jam Sysfs Path] ${MASCHINE_JAM_SYSFS_DIR} not found."
	echo "Exiting now."
	exit 2
fi

midi_map_io(){
	if [ $# -ne 4 ]; then
		echo "Incorrect number of parameters."
		echo "Usage: $(basename "$0") [sysfs directory] [channel] [type] [key]"
		echo "Exiting now."
		exit 1
	fi
	IO_DIRECTORY="$(readlink -m "$1")"
	if [ ! -d "$IO_DIRECTORY" ]; then
		echo "Directory ${IO_DIRECTORY} not found."
		return 2
	fi
	IO_CHANNEL="$2"
	if [ "$IO_CHANNEL" -lt 0 ] || [ "$IO_CHANNEL" -gt 15 ]; then
		echo "Invalid channel of ${IO_CHANNEL} given. Key must be 0-15."
		return 3
	fi
	IO_TYPE="$3"
	if [ "$IO_TYPE" != "note" ] && [ "$IO_TYPE" != "aftertouch" ] && [ "$IO_TYPE" != "control_change" ]; then
		echo "Invalid type of ${IO_TYPE} given. Type can be \"note\", \"aftertouch\", or \"control_change\"."
		return 4
	fi
	IO_KEY="$4"
	if [ "$IO_KEY" -lt 0 ] || [ "$IO_KEY" -gt 255 ]; then
		echo "Invalid key of ${IO_KEY} given. Key must be 0-255."
		return 5
	fi
	echo "$IO_CHANNEL" > "${IO_DIRECTORY}/channel"
	echo "$IO_TYPE" > "${IO_DIRECTORY}/type"
	echo "$IO_KEY" > "${IO_DIRECTORY}/key"

	NAME="$(basename ${IO_DIRECTORY})"
	UP_ONE="$(dirname "$IO_DIRECTORY")"
	UP_TWO="$(dirname "$UP_ONE")"
	echo "$(basename ${UP_TWO})/$(basename ${UP_ONE})/${NAME}: channel=${IO_CHANNEL}, type=${IO_TYPE}, key=${IO_KEY}"

	return 0
}

(cd "$MASCHINE_JAM_SYSFS_DIR"
	(cd inputs
		(cd buttons
			midi_map_io song 0 control_change 30
			midi_map_io step 0 control_change 31
			midi_map_io pad_mode 0 control_change 32
			midi_map_io clear 0 control_change 95
			midi_map_io duplicate 0 control_change 96
			midi_map_io navigate_up 0 control_change 40
			midi_map_io navigate_down 0 control_change 41
			midi_map_io navigate_left 0 control_change 42
			midi_map_io navigate_right 0 control_change 43

			midi_map_io scene_1 1 note 0
			midi_map_io scene_2 1 note 1
			midi_map_io scene_3 1 note 2
			midi_map_io scene_4 1 note 3
			midi_map_io scene_5 1 note 4
			midi_map_io scene_6 1 note 5
			midi_map_io scene_7 1 note 6
			midi_map_io scene_8 1 note 7
			midi_map_io group_a 1 note 8
			midi_map_io group_b 1 note 9
			midi_map_io group_c 1 note 10
			midi_map_io group_d 1 note 11
			midi_map_io group_e 1 note 12
			midi_map_io group_f 1 note 13
			midi_map_io group_g 1 note 14
			midi_map_io group_h 1 note 15
			midi_map_io matrix_1x1 0 note 22
			midi_map_io matrix_1x2 0 note 23
			midi_map_io matrix_1x3 0 note 24
			midi_map_io matrix_1x4 0 note 25
			midi_map_io matrix_1x5 0 note 26
			midi_map_io matrix_1x6 0 note 27
			midi_map_io matrix_1x7 0 note 28
			midi_map_io matrix_1x8 0 note 29
			midi_map_io matrix_2x1 0 note 30
			midi_map_io matrix_2x2 0 note 31
			midi_map_io matrix_2x3 0 note 32
			midi_map_io matrix_2x4 0 note 33
			midi_map_io matrix_2x5 0 note 34
			midi_map_io matrix_2x6 0 note 35
			midi_map_io matrix_2x7 0 note 36
			midi_map_io matrix_2x8 0 note 37
			midi_map_io matrix_3x1 0 note 38
			midi_map_io matrix_3x2 0 note 39
			midi_map_io matrix_3x3 0 note 40
			midi_map_io matrix_3x4 0 note 41
			midi_map_io matrix_3x5 0 note 42
			midi_map_io matrix_3x6 0 note 43
			midi_map_io matrix_3x7 0 note 44
			midi_map_io matrix_3x8 0 note 45
			midi_map_io matrix_4x1 0 note 46
			midi_map_io matrix_4x2 0 note 47
			midi_map_io matrix_4x3 0 note 48
			midi_map_io matrix_4x4 0 note 49
			midi_map_io matrix_4x5 0 note 50
			midi_map_io matrix_4x6 0 note 51
			midi_map_io matrix_4x7 0 note 52
			midi_map_io matrix_4x8 0 note 53
			midi_map_io matrix_5x1 0 note 54
			midi_map_io matrix_5x2 0 note 55
			midi_map_io matrix_5x3 0 note 56
			midi_map_io matrix_5x4 0 note 57
			midi_map_io matrix_5x5 0 note 58
			midi_map_io matrix_5x6 0 note 59
			midi_map_io matrix_5x7 0 note 60
			midi_map_io matrix_5x8 0 note 61
			midi_map_io matrix_6x1 0 note 62
			midi_map_io matrix_6x2 0 note 63
			midi_map_io matrix_6x3 0 note 64
			midi_map_io matrix_6x4 0 note 65
			midi_map_io matrix_6x5 0 note 66
			midi_map_io matrix_6x6 0 note 67
			midi_map_io matrix_6x7 0 note 68
			midi_map_io matrix_6x8 0 note 69
			midi_map_io matrix_7x1 0 note 70
			midi_map_io matrix_7x2 0 note 71
			midi_map_io matrix_7x3 0 note 72
			midi_map_io matrix_7x4 0 note 73
			midi_map_io matrix_7x5 0 note 74
			midi_map_io matrix_7x6 0 note 75
			midi_map_io matrix_7x7 0 note 76
			midi_map_io matrix_7x8 0 note 77
			midi_map_io matrix_8x1 0 note 78
			midi_map_io matrix_8x2 0 note 79
			midi_map_io matrix_8x3 0 note 80
			midi_map_io matrix_8x4 0 note 81
			midi_map_io matrix_8x5 0 note 82
			midi_map_io matrix_8x6 0 note 83
			midi_map_io matrix_8x7 0 note 84
			midi_map_io matrix_8x8 0 note 85

			midi_map_io mst 0 control_change 60
			midi_map_io grp 0 control_change 61
			midi_map_io in_1 0 control_change 62
			midi_map_io cue 0 control_change 63
			midi_map_io encoder_push 0 control_change 87
			midi_map_io encoder_touch 0 control_change 88
			midi_map_io browse 0 control_change 44

			midi_map_io macro 0 control_change 90
			midi_map_io level 0 control_change 91
			midi_map_io aux 0 control_change 92
			midi_map_io control 0 control_change 97
			midi_map_io auto 0 control_change 98

			midi_map_io perform 0 control_change 45
			midi_map_io notes 0 control_change 46
			midi_map_io lock 0 control_change 47
			midi_map_io tune 0 control_change 48
			midi_map_io swing 0 control_change 49
			midi_map_io select 0 control_change 80

			midi_map_io play 0 control_change 108
			midi_map_io rec 0 control_change 109
			midi_map_io page_left 0 control_change 107
			midi_map_io page_right 0 control_change 104
			midi_map_io tempo 0 control_change 110
			midi_map_io grid 0 control_change 113
			midi_map_io solo 0 control_change 111
			midi_map_io mute 0 control_change 112
		)
		(cd knobs
			midi_map_io encoder 0 control_change 86
		)
		(cd smartstrips
			midi_map_io 1AS 0 control_change 8
			midi_map_io 2AS 0 control_change 9
			midi_map_io 3AS 0 control_change 10
			midi_map_io 4AS 0 control_change 11
			midi_map_io 5AS 0 control_change 12
			midi_map_io 6AS 0 control_change 13
			midi_map_io 7AS 0 control_change 14
			midi_map_io 8AS 0 control_change 15

			midi_map_io 1BS 0 control_change 16
			midi_map_io 2BS 0 control_change 17
			midi_map_io 3BS 0 control_change 18
			midi_map_io 4BS 0 control_change 19
			midi_map_io 5BS 0 control_change 20
			midi_map_io 6BS 0 control_change 21
			midi_map_io 7BS 0 control_change 22
			midi_map_io 8BS 0 control_change 23
		)
	)
	(cd outputs
		(cd buttons
			midi_map_io song 0 control_change 30
			midi_map_io step 0 control_change 31
			midi_map_io pad_mode 0 control_change 32
			midi_map_io clear 0 control_change 95
			midi_map_io duplicate 0 control_change 96
			midi_map_io navigate_up 0 control_change 40
			midi_map_io navigate_down 0 control_change 41
			midi_map_io navigate_left 0 control_change 42
			midi_map_io navigate_right 0 control_change 43

			midi_map_io mst 0 control_change 60
			midi_map_io grp 0 control_change 61
			midi_map_io in_1 0 control_change 62
			midi_map_io cue 0 control_change 63
			midi_map_io browse 0 control_change 44

			midi_map_io macro 0 control_change 90
			midi_map_io level 0 control_change 91
			midi_map_io aux 0 control_change 92
			midi_map_io control 0 control_change 97
			midi_map_io auto 0 control_change 98

			midi_map_io perform 0 control_change 45
			midi_map_io notes 0 control_change 46
			midi_map_io lock 0 control_change 47
			midi_map_io tune 0 control_change 48
			midi_map_io swing 0 control_change 49
			midi_map_io select 0 control_change 80

			midi_map_io play 0 control_change 108
			midi_map_io rec 0 control_change 109
			midi_map_io page_left 0 control_change 107
			midi_map_io page_right 0 control_change 104
			midi_map_io tempo 0 control_change 110
			midi_map_io grid 0 control_change 113
			midi_map_io solo 0 control_change 111
			midi_map_io mute 0 control_change 112

			midi_map_io level_left_1 0 control_change 8
			midi_map_io level_left_2 0 control_change 9
			midi_map_io level_left_3 0 control_change 10
			midi_map_io level_left_4 0 control_change 11
			midi_map_io level_left_5 0 control_change 12
			midi_map_io level_left_6 0 control_change 13
			midi_map_io level_left_7 0 control_change 14
			midi_map_io level_left_8 0 control_change 15
			midi_map_io level_right_1 0 control_change 20
			midi_map_io level_right_2 0 control_change 21
			midi_map_io level_right_3 0 control_change 22
			midi_map_io level_right_4 0 control_change 23
			midi_map_io level_right_5 0 control_change 24
			midi_map_io level_right_6 0 control_change 25
			midi_map_io level_right_7 0 control_change 26
			midi_map_io level_right_8 0 control_change 27

			midi_map_io scene_1 1 note 0
			midi_map_io scene_2 1 note 1
			midi_map_io scene_3 1 note 2
			midi_map_io scene_4 1 note 3
			midi_map_io scene_5 1 note 4
			midi_map_io scene_6 1 note 5
			midi_map_io scene_7 1 note 6
			midi_map_io scene_8 1 note 7
			midi_map_io group_a 1 note 8
			midi_map_io group_b 1 note 9
			midi_map_io group_c 1 note 10
			midi_map_io group_d 1 note 11
			midi_map_io group_e 1 note 12
			midi_map_io group_f 1 note 13
			midi_map_io group_g 1 note 14
			midi_map_io group_h 1 note 15
			midi_map_io matrix_1x1 0 note 22
			midi_map_io matrix_1x2 0 note 23
			midi_map_io matrix_1x3 0 note 24
			midi_map_io matrix_1x4 0 note 25
			midi_map_io matrix_1x5 0 note 26
			midi_map_io matrix_1x6 0 note 27
			midi_map_io matrix_1x7 0 note 28
			midi_map_io matrix_1x8 0 note 29
			midi_map_io matrix_2x1 0 note 30
			midi_map_io matrix_2x2 0 note 31
			midi_map_io matrix_2x3 0 note 32
			midi_map_io matrix_2x4 0 note 33
			midi_map_io matrix_2x5 0 note 34
			midi_map_io matrix_2x6 0 note 35
			midi_map_io matrix_2x7 0 note 36
			midi_map_io matrix_2x8 0 note 37
			midi_map_io matrix_3x1 0 note 38
			midi_map_io matrix_3x2 0 note 39
			midi_map_io matrix_3x3 0 note 40
			midi_map_io matrix_3x4 0 note 41
			midi_map_io matrix_3x5 0 note 42
			midi_map_io matrix_3x6 0 note 43
			midi_map_io matrix_3x7 0 note 44
			midi_map_io matrix_3x8 0 note 45
			midi_map_io matrix_4x1 0 note 46
			midi_map_io matrix_4x2 0 note 47
			midi_map_io matrix_4x3 0 note 48
			midi_map_io matrix_4x4 0 note 49
			midi_map_io matrix_4x5 0 note 50
			midi_map_io matrix_4x6 0 note 51
			midi_map_io matrix_4x7 0 note 52
			midi_map_io matrix_4x8 0 note 53
			midi_map_io matrix_5x1 0 note 54
			midi_map_io matrix_5x2 0 note 55
			midi_map_io matrix_5x3 0 note 56
			midi_map_io matrix_5x4 0 note 57
			midi_map_io matrix_5x5 0 note 58
			midi_map_io matrix_5x6 0 note 59
			midi_map_io matrix_5x7 0 note 60
			midi_map_io matrix_5x8 0 note 61
			midi_map_io matrix_6x1 0 note 62
			midi_map_io matrix_6x2 0 note 63
			midi_map_io matrix_6x3 0 note 64
			midi_map_io matrix_6x4 0 note 65
			midi_map_io matrix_6x5 0 note 66
			midi_map_io matrix_6x6 0 note 67
			midi_map_io matrix_6x7 0 note 68
			midi_map_io matrix_6x8 0 note 69
			midi_map_io matrix_7x1 0 note 70
			midi_map_io matrix_7x2 0 note 71
			midi_map_io matrix_7x3 0 note 72
			midi_map_io matrix_7x4 0 note 73
			midi_map_io matrix_7x5 0 note 74
			midi_map_io matrix_7x6 0 note 75
			midi_map_io matrix_7x7 0 note 76
			midi_map_io matrix_7x8 0 note 77
			midi_map_io matrix_8x1 0 note 78
			midi_map_io matrix_8x2 0 note 79
			midi_map_io matrix_8x3 0 note 80
			midi_map_io matrix_8x4 0 note 81
			midi_map_io matrix_8x5 0 note 82
			midi_map_io matrix_8x6 0 note 83
			midi_map_io matrix_8x7 0 note 84
			midi_map_io matrix_8x8 0 note 85
		)
		(cd smartstrips
			#purely for the purpose of testing!
			midi_map_io led_1x01 0 control_change 38
			midi_map_io led_1x11 0 control_change 39
			midi_map_io led_4x01 0 note 82
			midi_map_io led_4x11 0 note 83
			midi_map_io led_5x04 0 note 84
			midi_map_io led_5x06 0 note 85
			midi_map_io led_8x01 0 control_change 8
			midi_map_io led_8x11 0 control_change 9
		)
	)
)
#note = 9
#aftertouch = 10
#cc = 11
echo "Successfully mapped Maschine Jam for Bitwig."
