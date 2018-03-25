#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/hid.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>
#include <sound/asequencer.h>
#include <sound/seq_midi_event.h>

#include "hid-ids.h"

#define MASCHINE_JAM_HID_REPORT_ID_BYTES 1
#define MASCHINE_JAM_NUMBER_KNOBS 2
#define MASCHINE_JAM_HID_REPORT_01_KNOB_BITS 4
#define MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES (MASCHINE_JAM_NUMBER_KNOBS * MASCHINE_JAM_HID_REPORT_01_KNOB_BITS) / 8 // 1
#define MASCHINE_JAM_NUMBER_BUTTONS 120
#define MASCHINE_JAM_HID_REPORT_01_BUTTON_BITS 1
#define MASCHINE_JAM_HID_REPORT_01_BUTTONS_BYTES (MASCHINE_JAM_NUMBER_BUTTONS * MASCHINE_JAM_HID_REPORT_01_BUTTON_BITS) / 8 // 15
#define MASCHINE_JAM_HID_REPORT_01_DATA_BYTES (MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES + MASCHINE_JAM_HID_REPORT_01_BUTTONS_BYTES) // 16
#define MASCHINE_JAM_HID_REPORT_01_BYTES (MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_HID_REPORT_01_DATA_BYTES) // 17
#define MASCHINE_JAM_NUMBER_SMARTSTRIPS 8
#define MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGERS 2
enum maschine_jam_smartstrip_finger_mode{
	MJ_SMARTSTRIP_FINGER_MODE_TOUCH = 0,
	MJ_SMARTSTRIP_FINGER_MODE_SLIDE = 1
};
#define MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGER_MODES 2
#define MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_TIMESTAMP_BITS 16
#define MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_TOUCH_VALUE_BITS 16
#define MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_BITS (MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_TIMESTAMP_BITS + (MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_TOUCH_VALUE_BITS * 2)) // 48
#define MASCHINE_JAM_HID_REPORT_02_SMARTSTRIPS_BYTES (MASCHINE_JAM_NUMBER_SMARTSTRIPS * MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_BITS) / 8 // 48
#define MASCHINE_JAM_HID_REPORT_02_DATA_BYTES (MASCHINE_JAM_HID_REPORT_02_SMARTSTRIPS_BYTES) // 48
#define MASCHINE_JAM_HID_REPORT_02_BYTES (MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_HID_REPORT_02_DATA_BYTES) // 49

#define MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS VERIFY_OCTAL_PERMISSIONS(0664)

#define MASCHINE_JAM_SYSEX_MAX_LENGTH 28
#define MASCHINE_JAM_MIDI_CHANNELS_MAX 16
#define MASCHINE_JAM_MIDI_NOTES_MAX 128
#define MASCHINE_JAM_MIDI_CONTROL_CHANGE_PARAMS_MAX 128
enum maschine_jam_midi_type{
	MJ_MIDI_TYPE_NOTE,
	MJ_MIDI_TYPE_AFTERTOUCH,
	MJ_MIDI_TYPE_CONTROL_CHANGE,
	MJ_MIDI_TYPE_SYSEX
};
#define MJ_MIDI_TYPE_NOTE_STRING "note\n"
#define MJ_MIDI_TYPE_AFTERTOUCH_STRING "aftertouch\n"
#define MJ_MIDI_TYPE_CONTROL_CHANGE_STRING "control_change\n"

#define MASCHINE_JAM_NUMBER_BUTTON_LEDS 53
#define MASCHINE_JAM_NUMBER_PAD_LEDS 80
#define MASCHINE_JAM_NUMBER_LEDS_PER_SMARTSTRIP 11
#define MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS (MASCHINE_JAM_NUMBER_SMARTSTRIPS * MASCHINE_JAM_NUMBER_LEDS_PER_SMARTSTRIP)

struct maschine_jam_midi_config {
	enum maschine_jam_midi_type type;
	uint8_t channel; // lower nibble 0-15
	uint8_t key; // 0-127
	uint8_t value_min; // 0
	uint8_t value_max; // 127
};

enum maschine_jam_output_type{
	MJ_OUTPUT_NOTE_MAPPING_SENTINAL,
	MJ_OUTPUT_CONTROL_CHANGE_MAPPING_SENTINAL,
	MJ_OUTPUT_BUTTON_LED_NODE,
	MJ_OUTPUT_PAD_LED_NODE,
	MJ_OUTPUT_SMARTSTRIP_LED_NODE
};
struct maschine_jam_output_node {
	enum maschine_jam_output_type type;
	union {
		struct { // MJ_OUTPUT_NOTE_MAPPING_SENTINAL || MJ_OUTPUT_CONTROL_CHANGE_MAPPING_SENTINAL
			uint8_t channel;
			union {
				uint8_t note; // MJ_OUTPUT_NOTE_MAPPING_SENTINAL
				uint8_t param; // MJ_OUTPUT_CONTROL_CHANGE_MAPPING_SENTINAL
			};
			struct maschine_jam_output_node* node_list_head;
		};
		struct { // MJ_OUTPUT_BUTTON_LED_NODE || MJ_OUTPUT_PAD_LED_NODE || MJ_OUTPUT_SMARTSTRIP_LED_NODE
			uint8_t index;
			struct maschine_jam_output_node* previous;
			struct maschine_jam_output_node* next;
		};
	};
};

enum maschine_jam_smartstrip_display_mode{
	MJ_SMARTSTRIP_DISPLAY_MODE_SINGLE = 0x00,
	MJ_SMARTSTRIP_DISPLAY_MODE_DOT = 0x01,
	MJ_SMARTSTRIP_DISPLAY_MODE_PAN = 0x02,
	MJ_SMARTSTRIP_DISPLAY_MODE_DUAL = 0x03,
};
struct maschine_jam_smartstrip_display_state {
	enum maschine_jam_smartstrip_display_mode mode;
	uint8_t color;
	uint8_t value;
};

struct maschine_jam_driver_data {
	// Device Information
	struct hid_device 		*mj_hid_device;

	// Inputs
	struct maschine_jam_midi_config	midi_in_knob_configs[MASCHINE_JAM_NUMBER_KNOBS];
	uint8_t					hid_report01_data_knobs[MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES];
	struct maschine_jam_midi_config	midi_in_button_configs[MASCHINE_JAM_NUMBER_BUTTONS];
	uint8_t					hid_report01_data_buttons[MASCHINE_JAM_HID_REPORT_01_BUTTONS_BYTES];
	struct maschine_jam_midi_config	midi_in_smartstrip_configs[MASCHINE_JAM_NUMBER_SMARTSTRIPS][MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGERS][MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGER_MODES];
	uint8_t					hid_report02_data_smartstrips[MASCHINE_JAM_HID_REPORT_02_BYTES];

	// Outputs
	struct maschine_jam_output_node midi_out_note_mapping[MASCHINE_JAM_MIDI_CHANNELS_MAX][MASCHINE_JAM_MIDI_NOTES_MAX];
	struct maschine_jam_output_node midi_out_control_change_mapping[MASCHINE_JAM_MIDI_CHANNELS_MAX][MASCHINE_JAM_MIDI_CONTROL_CHANGE_PARAMS_MAX];
	struct maschine_jam_output_node midi_out_button_led_nodes[MASCHINE_JAM_NUMBER_BUTTON_LEDS];
	struct maschine_jam_output_node midi_out_pad_led_nodes[MASCHINE_JAM_NUMBER_PAD_LEDS];
	struct maschine_jam_output_node midi_out_smartstrip_led_nodes[MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS];
	spinlock_t				midi_out_mapping_lock;
	uint8_t					hid_report_led_buttons[MASCHINE_JAM_NUMBER_BUTTON_LEDS];
	spinlock_t				hid_report_led_buttons_lock;
	struct work_struct		hid_report_led_buttons_work;
	uint8_t					hid_report_led_pads[MASCHINE_JAM_NUMBER_PAD_LEDS];
	spinlock_t				hid_report_led_pads_lock;
	struct work_struct		hid_report_led_pads_work;
	uint8_t					hid_report_led_smartstrips[MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS];
	struct maschine_jam_smartstrip_display_state hid_report_led_smartstrips_display_states[MASCHINE_JAM_NUMBER_SMARTSTRIPS];
	spinlock_t				hid_report_led_smartstrips_lock;
	struct work_struct		hid_report_led_smartstrips_work;

	// Sysfs Interface
	struct kobject *directory_inputs;
	struct kobject *directory_inputs_knobs;
	struct kobject *directory_inputs_buttons;
	struct kobject *directory_inputs_smartstrips;
	struct kobject *directory_outputs;
	struct kobject *directory_outputs_button_leds;
	struct kobject *directory_outputs_pad_leds;
	struct kobject *directory_outputs_smartstrip_leds;

	// Sound/Midi Interface
	struct snd_card			*sound_card;
	int						sound_card_device_number;
	struct snd_rawmidi		*rawmidi_interface;
	struct snd_rawmidi_substream	*midi_in_substream;
	unsigned long			midi_in_up;
	spinlock_t				midi_in_lock;
	struct snd_midi_event*	midi_in_decoder;
	spinlock_t				midi_in_decoder_lock;
	struct snd_rawmidi_substream	*midi_out_substream;
	unsigned long			midi_out_up;
	spinlock_t				midi_out_lock;
	struct snd_midi_event*	midi_out_encoder;
	spinlock_t				midi_out_encoder_lock;
};

static void maschine_jam_hid_write_led_buttons_report(struct work_struct *);
static void maschine_jam_hid_write_led_pads_report(struct work_struct *);
static void maschine_jam_hid_write_led_smartstrips_report(struct work_struct *);
static int8_t maschine_jam_output_mapping_add(struct maschine_jam_output_node*, struct maschine_jam_output_node*);
static void maschine_jam_initialize_driver_data(struct maschine_jam_driver_data *driver_data, struct hid_device *mj_hid_device){
	unsigned int i, j, k, temp_key;

	// HID Device
	driver_data->mj_hid_device = mj_hid_device;

	// Inputs
	temp_key = 0;
	for(i = 0; i < MASCHINE_JAM_NUMBER_KNOBS; i++){
		driver_data->midi_in_knob_configs[i].type = MJ_MIDI_TYPE_NOTE;
		driver_data->midi_in_knob_configs[i].channel = 0;
		driver_data->midi_in_knob_configs[i].key = temp_key;
		driver_data->midi_in_knob_configs[i].value_min = 0;
		driver_data->midi_in_knob_configs[i].value_max = 0x7F; // 127
		temp_key++;
	}
	memset(driver_data->hid_report01_data_knobs, 0, sizeof(driver_data->hid_report01_data_knobs));
	for(i = 0; i < MASCHINE_JAM_NUMBER_BUTTONS; i++){
		driver_data->midi_in_button_configs[i].type = MJ_MIDI_TYPE_NOTE;
		driver_data->midi_in_button_configs[i].channel = 0;
		driver_data->midi_in_button_configs[i].key = temp_key;
		driver_data->midi_in_button_configs[i].value_min = 0;
		driver_data->midi_in_button_configs[i].value_max = 0x7F; // 127
		temp_key++;
	}
	memset(driver_data->hid_report01_data_buttons, 0, sizeof(driver_data->hid_report01_data_buttons));
	temp_key = 0;
	for(i = 0; i < MASCHINE_JAM_NUMBER_SMARTSTRIPS; i++){
		for(j=0; j < MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGERS; j++){
			for(k=0; k < MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGER_MODES; k++){
				driver_data->midi_in_smartstrip_configs[i][j][k].type = MJ_MIDI_TYPE_CONTROL_CHANGE;
				driver_data->midi_in_smartstrip_configs[i][j][k].channel = 1;
				driver_data->midi_in_smartstrip_configs[i][j][k].key = temp_key;
				driver_data->midi_in_smartstrip_configs[i][j][k].value_min = 0;
				driver_data->midi_in_smartstrip_configs[i][j][k].value_max = 0x7F; // 127
				temp_key++;
			}
		}
	}
	memset(driver_data->hid_report02_data_smartstrips, 0, sizeof(driver_data->hid_report02_data_smartstrips));

	// Outputs
	for(i=0;i<MASCHINE_JAM_MIDI_CHANNELS_MAX;i++){
		for(j=0;j<MASCHINE_JAM_MIDI_NOTES_MAX;j++){
			driver_data->midi_out_note_mapping[i][j].type = MJ_OUTPUT_NOTE_MAPPING_SENTINAL;
			driver_data->midi_out_note_mapping[i][j].channel = i;
			driver_data->midi_out_note_mapping[i][j].note = j;
			driver_data->midi_out_note_mapping[i][j].node_list_head = NULL;
		}
	}
	for(i=0;i<MASCHINE_JAM_MIDI_CHANNELS_MAX;i++){
		for(j=0;j<MASCHINE_JAM_MIDI_CONTROL_CHANGE_PARAMS_MAX;j++){
			driver_data->midi_out_control_change_mapping[i][j].type = MJ_OUTPUT_CONTROL_CHANGE_MAPPING_SENTINAL;
			driver_data->midi_out_control_change_mapping[i][j].channel = i;
			driver_data->midi_out_control_change_mapping[i][j].param = j;
			driver_data->midi_out_control_change_mapping[i][j].node_list_head = NULL;
		}
	}
	for(i=0;i<MASCHINE_JAM_NUMBER_BUTTON_LEDS;i++){
		driver_data->midi_out_button_led_nodes[i].type = MJ_OUTPUT_BUTTON_LED_NODE;
		driver_data->midi_out_button_led_nodes[i].index = i;
		driver_data->midi_out_button_led_nodes[i].previous = NULL;
		driver_data->midi_out_button_led_nodes[i].next = NULL;
		maschine_jam_output_mapping_add(&driver_data->midi_out_note_mapping[0][0], &driver_data->midi_out_button_led_nodes[i]);
	}
	for(i=0;i<MASCHINE_JAM_NUMBER_PAD_LEDS;i++){
		driver_data->midi_out_pad_led_nodes[i].type = MJ_OUTPUT_PAD_LED_NODE;
		driver_data->midi_out_pad_led_nodes[i].index = i;
		driver_data->midi_out_pad_led_nodes[i].previous = NULL;
		driver_data->midi_out_pad_led_nodes[i].next = NULL;
		maschine_jam_output_mapping_add(&driver_data->midi_out_note_mapping[0][0], &driver_data->midi_out_pad_led_nodes[i]);
	}
	for(i=0;i<MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS;i++){
		driver_data->midi_out_smartstrip_led_nodes[i].type = MJ_OUTPUT_SMARTSTRIP_LED_NODE;
		driver_data->midi_out_smartstrip_led_nodes[i].index = i;
		driver_data->midi_out_smartstrip_led_nodes[i].previous = NULL;
		driver_data->midi_out_smartstrip_led_nodes[i].next = NULL;
		maschine_jam_output_mapping_add(&driver_data->midi_out_note_mapping[0][0], &driver_data->midi_out_smartstrip_led_nodes[i]);
	}
	spin_lock_init(&driver_data->midi_out_mapping_lock);
	memset(driver_data->hid_report_led_buttons, 0, sizeof(driver_data->hid_report_led_buttons));
	spin_lock_init(&driver_data->hid_report_led_buttons_lock);
	INIT_WORK(&driver_data->hid_report_led_buttons_work, maschine_jam_hid_write_led_buttons_report);
	memset(driver_data->hid_report_led_pads, 0, sizeof(driver_data->hid_report_led_pads));
	spin_lock_init(&driver_data->hid_report_led_pads_lock);
	INIT_WORK(&driver_data->hid_report_led_pads_work, maschine_jam_hid_write_led_pads_report);
	memset(driver_data->hid_report_led_smartstrips, 0, sizeof(driver_data->hid_report_led_smartstrips));
	spin_lock_init(&driver_data->hid_report_led_smartstrips_lock);
	INIT_WORK(&driver_data->hid_report_led_smartstrips_work, maschine_jam_hid_write_led_smartstrips_report);

	// Sysfs Interface
	driver_data->directory_inputs = NULL;
	driver_data->directory_inputs_knobs = NULL;
	driver_data->directory_inputs_buttons = NULL;
	driver_data->directory_inputs_smartstrips = NULL;
	driver_data->directory_outputs = NULL;
	driver_data->directory_outputs_button_leds = NULL;
	driver_data->directory_outputs_pad_leds = NULL;
	driver_data->directory_outputs_smartstrip_leds = NULL;

	// Sound/Midi Interface
	driver_data->sound_card = NULL;
	driver_data->rawmidi_interface = NULL;
	driver_data->midi_in_substream = NULL;
	driver_data->midi_in_up = 0;
	spin_lock_init(&driver_data->midi_in_lock);
	spin_lock_init(&driver_data->midi_in_decoder_lock);
	driver_data->midi_out_substream = NULL;
	driver_data->midi_out_up = 0;
	spin_lock_init(&driver_data->midi_out_lock);
	spin_lock_init(&driver_data->midi_out_encoder_lock);
}

static int8_t maschine_jam_output_mapping_add(struct maschine_jam_output_node* mapping_sentinal, struct maschine_jam_output_node* output_node){
	struct maschine_jam_output_node* current_node;

	if (output_node->previous != NULL || output_node->next != NULL){
		printk(KERN_ALERT "output_node must be removed before it can be added.\n");
		return -1;
	}
	if (mapping_sentinal->node_list_head == NULL){
		mapping_sentinal->node_list_head = output_node;
		output_node->previous = mapping_sentinal;
	}else{
		current_node = mapping_sentinal->node_list_head;
		while (current_node->next != NULL){
			current_node = current_node->next;
		}
		current_node->next = output_node;
		output_node->previous = current_node;
	}
	return 0;
}
static void maschine_jam_output_mapping_remove(struct maschine_jam_output_node* output_node){
	if (output_node->previous != NULL){
		if (output_node->previous->type == MJ_OUTPUT_NOTE_MAPPING_SENTINAL || output_node->previous->type == MJ_OUTPUT_CONTROL_CHANGE_MAPPING_SENTINAL){
			output_node->previous->node_list_head = output_node->next;
		}else{
			output_node->previous->next = output_node->next;
		}
		if (output_node->next != NULL){
			output_node->next->previous = output_node->previous;
		}
	}
	output_node->previous = NULL;
	output_node->next = NULL;
}
static inline struct maschine_jam_output_node* maschine_jam_output_mapping_get_sentinal(struct maschine_jam_output_node* output_node){
	struct maschine_jam_output_node* current_node;

	if (output_node->previous == NULL){
		return NULL;
	}else{
		current_node = output_node->previous;
		while(current_node->type == MJ_OUTPUT_BUTTON_LED_NODE || \
			current_node->type == MJ_OUTPUT_PAD_LED_NODE || \
			current_node->type == MJ_OUTPUT_SMARTSTRIP_LED_NODE)
		{
			current_node = current_node->previous;
		}
		if(current_node->type == MJ_OUTPUT_NOTE_MAPPING_SENTINAL || \
			current_node->type == MJ_OUTPUT_CONTROL_CHANGE_MAPPING_SENTINAL)
		{
			return current_node;
		}else{
			return NULL;
		}
	}
}
static int8_t maschine_jam_output_mapping_get_midi_info(struct maschine_jam_output_node* output_node, struct snd_seq_event* return_midi_event){
	struct maschine_jam_output_node* sentinal_node;

	sentinal_node = maschine_jam_output_mapping_get_sentinal(output_node);
	if(sentinal_node == NULL){
		printk(KERN_ALERT  "maschine_jam_output_mapping_get_midi_info: sentinal node not found\n");
		return -1;
	}else{
		if(sentinal_node->type == MJ_OUTPUT_NOTE_MAPPING_SENTINAL){
			return_midi_event->type = SNDRV_SEQ_EVENT_NOTE;
			return_midi_event->data.note.channel = sentinal_node->channel;
			return_midi_event->data.note.note = sentinal_node->note;
			return 0;
		}else if(sentinal_node->type == MJ_OUTPUT_CONTROL_CHANGE_MAPPING_SENTINAL){
			return_midi_event->type = SNDRV_SEQ_EVENT_CONTROLLER;
			return_midi_event->data.control.channel = sentinal_node->channel;
			return_midi_event->data.control.param = sentinal_node->param;
			return 0;
		}else{
			printk(KERN_ALERT  "maschine_jam_output_mapping_get_midi_info: invalid sentinal_node->type\n");
			return -2;
		}
	}
}
static int8_t maschine_jam_output_mapping_set_midi_info(struct maschine_jam_driver_data* driver_data, struct maschine_jam_output_node* output_node, struct snd_seq_event* midi_event){
	maschine_jam_output_mapping_remove(output_node);
	if(midi_event->type == SNDRV_SEQ_EVENT_NOTE){
		return 	maschine_jam_output_mapping_add(&driver_data->midi_out_note_mapping[midi_event->data.note.channel][midi_event->data.note.note], output_node);
	}else if(midi_event->type == SNDRV_SEQ_EVENT_CONTROLLER){
		return 	maschine_jam_output_mapping_add(&driver_data->midi_out_control_change_mapping[midi_event->data.control.channel][midi_event->data.control.param], output_node);
	}else{
		printk(KERN_ALERT "maschine_jam_output_mapping_set_midi_info - invalid midi_event->type\n");
		return -1;
	}
}
inline static int8_t maschine_jam_snd_seq_event_get_channel(struct snd_seq_event* midi_event){
	if(midi_event->type == SNDRV_SEQ_EVENT_NOTE){
		return 	midi_event->data.note.channel;
	}else if(midi_event->type == SNDRV_SEQ_EVENT_CONTROLLER){
		return 	midi_event->data.control.channel;
	}else{
		printk(KERN_ALERT "maschine_jam_snd_seq_event_get_channel: invalid midi_event->type\n");
		return -1;
	}
}
inline static void maschine_jam_snd_seq_event_set_channel(struct snd_seq_event* midi_event, uint8_t channel){
	if(midi_event->type == SNDRV_SEQ_EVENT_NOTE){
		midi_event->data.note.channel = channel;
	}else if(midi_event->type == SNDRV_SEQ_EVENT_CONTROLLER){
		midi_event->data.control.channel = channel;
	}else{
		printk(KERN_ALERT "maschine_jam_snd_seq_event_set_channel: invalid midi_event->type\n");
	}
}
inline static int8_t maschine_jam_snd_seq_event_get_key(struct snd_seq_event* midi_event){
	if(midi_event->type == SNDRV_SEQ_EVENT_NOTE){
		return 	midi_event->data.note.note;
	}else if(midi_event->type == SNDRV_SEQ_EVENT_CONTROLLER){
		return 	midi_event->data.control.param;
	}else{
		printk(KERN_ALERT "maschine_jam_snd_seq_event_get_key: invalid midi_event->type\n");
		return -1;
	}
}
inline static void maschine_jam_snd_seq_event_set_key(struct snd_seq_event* midi_event, uint8_t key){
	if(midi_event->type == SNDRV_SEQ_EVENT_NOTE){
		midi_event->data.note.note = key;
	}else if(midi_event->type == SNDRV_SEQ_EVENT_CONTROLLER){
		midi_event->data.control.param = key;
	}else{
		printk(KERN_ALERT "maschine_jam_snd_seq_event_set_key: invalid midi_event->type\n");
	}
}

static void maschine_jam_hid_write_led_buttons_report(struct work_struct *work){
	int ret = 0;
	struct maschine_jam_driver_data *driver_data = container_of(work, struct maschine_jam_driver_data, hid_report_led_buttons_work);
	unsigned char *buffer;

	buffer = kzalloc(MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_NUMBER_BUTTON_LEDS, GFP_KERNEL);
	buffer[0] = 0x80;
	spin_lock(&driver_data->hid_report_led_buttons_lock);
	memcpy(&buffer[MASCHINE_JAM_HID_REPORT_ID_BYTES], &driver_data->hid_report_led_buttons, MASCHINE_JAM_NUMBER_BUTTON_LEDS);
	spin_unlock(&driver_data->hid_report_led_buttons_lock);
	ret = hid_hw_output_report(driver_data->mj_hid_device, buffer, MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_NUMBER_BUTTON_LEDS);
	//printk(KERN_NOTICE "maschine_jam_hid_write_report() - 0x80, ret=%d", ret);
	kfree(buffer);
}
static void maschine_jam_hid_write_led_pads_report(struct work_struct *work){
	int ret = 0;
	struct maschine_jam_driver_data *driver_data = container_of(work, struct maschine_jam_driver_data, hid_report_led_pads_work);
	unsigned char *buffer;

	buffer = kzalloc(MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_NUMBER_PAD_LEDS, GFP_KERNEL);
	buffer[0] = 0x81;
	spin_lock(&driver_data->hid_report_led_pads_lock);
	memcpy(&buffer[MASCHINE_JAM_HID_REPORT_ID_BYTES], &driver_data->hid_report_led_pads, MASCHINE_JAM_NUMBER_PAD_LEDS);
	spin_unlock(&driver_data->hid_report_led_pads_lock);
	ret = hid_hw_output_report(driver_data->mj_hid_device, buffer, MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_NUMBER_PAD_LEDS);
	//printk(KERN_NOTICE "maschine_jam_hid_write_report() - 0x81, ret=%d", ret);
	kfree(buffer);
}
static void maschine_jam_hid_write_led_smartstrips_report(struct work_struct *work){
	int ret = 0;
	struct maschine_jam_driver_data *driver_data = container_of(work, struct maschine_jam_driver_data, hid_report_led_smartstrips_work);
	unsigned char *buffer;

	buffer = kzalloc(MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS, GFP_KERNEL);
	buffer[0] = 0x82;
	spin_lock(&driver_data->hid_report_led_smartstrips_lock);
	memcpy(&buffer[MASCHINE_JAM_HID_REPORT_ID_BYTES], &driver_data->hid_report_led_smartstrips, MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS);
	spin_unlock(&driver_data->hid_report_led_smartstrips_lock);
	ret = hid_hw_output_report(driver_data->mj_hid_device, buffer, MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS);
	//printk(KERN_NOTICE "maschine_jam_hid_write_report() - 0x82, ret=%d", ret);
	kfree(buffer);
}

static int maschine_jam_write_snd_seq_event(struct maschine_jam_driver_data *driver_data, struct snd_seq_event* event){
	int8_t bytes_transmitted = 0, message_size = 0;
	unsigned long flags;
	unsigned char buffer[MASCHINE_JAM_SYSEX_MAX_LENGTH];

	spin_lock_irqsave(&driver_data->midi_in_decoder_lock, flags);
	message_size = snd_midi_event_decode(driver_data->midi_in_decoder, buffer, MASCHINE_JAM_SYSEX_MAX_LENGTH, event);
	spin_unlock_irqrestore(&driver_data->midi_in_decoder_lock, flags);

	if (message_size <= 0){
		printk(KERN_ALERT "maschine_jam_write_snd_seq_event: message_size: %d <= 0\n", message_size);
		return 0;
	}

	if (driver_data->midi_in_substream != NULL){
		spin_lock_irqsave(&driver_data->midi_in_lock, flags);
		bytes_transmitted = snd_rawmidi_receive(driver_data->midi_in_substream, buffer, message_size);
		spin_unlock_irqrestore(&driver_data->midi_in_lock, flags);
	}else{
		printk(KERN_ALERT "maschine_jam_write_snd_seq_event: no midi_in_substeam to write to.");
	}
	printk(KERN_NOTICE "maschine_jam_write_snd_seq_event: message_size: %d, bytes_transmitted: %d\n", message_size, bytes_transmitted);

	return bytes_transmitted;
}
static int maschine_jam_write_midi_event(struct maschine_jam_driver_data *driver_data,
	enum maschine_jam_midi_type midi_type, uint8_t channel, uint8_t key, uint8_t value){
	struct snd_seq_event event;

	switch(midi_type){
		case MJ_MIDI_TYPE_NOTE:
			printk(KERN_NOTICE "maschine_jam_write_midi_event: noteon: %d %d %d\n", channel, key, value);
			event.type = SNDRV_SEQ_EVENT_NOTEON;
			event.data.note.channel = channel;
			event.data.note.note = key;
			event.data.note.velocity = value;
			break;
		case MJ_MIDI_TYPE_AFTERTOUCH:
			printk(KERN_NOTICE "maschine_jam_write_midi_event: aftertouch: %d %d %d\n", channel, key, value);
			event.type = SNDRV_SEQ_EVENT_KEYPRESS;
			event.data.note.channel = channel;
			event.data.note.note = key;
			event.data.note.velocity = value;
			break;
		case MJ_MIDI_TYPE_CONTROL_CHANGE:
			printk(KERN_NOTICE "maschine_jam_write_midi_event: control_change: %d %d %d\n", channel, key, value);
			event.type = SNDRV_SEQ_EVENT_CONTROLLER;
			event.data.control.channel = channel;
			event.data.control.param = key;
			event.data.control.value = value;
			break;
		default:
			printk(KERN_ALERT "maschine_jam_write_midi_event: invalid midi type.\n");
			return 0;
			break;
	}
	return maschine_jam_write_snd_seq_event(driver_data, &event);
}
static int maschine_jam_write_sysex_event(struct maschine_jam_driver_data *driver_data, unsigned char* message, uint8_t message_length){
	struct snd_seq_event event;

	printk(KERN_NOTICE "maschine_jam_write_sysex_event: length: %d. message: %02X%02X%02X%02X\n", message_length, message[0], message[1], message[2], message[3]);
	event.type = SNDRV_SEQ_EVENT_SYSEX;
	event.flags = 0;
	event.flags &= ~SNDRV_SEQ_EVENT_LENGTH_MASK;
	event.flags |= SNDRV_SEQ_EVENT_LENGTH_VARIABLE;
	event.data.ext.ptr = message;
	event.data.ext.len = message_length;

	return maschine_jam_write_snd_seq_event(driver_data, &event);
}

static inline uint8_t maschine_jam_get_knob_nibble(u8 *data, uint8_t offset){
	return (data[offset / 2] >> ((offset % 2) * 4)) & 0x0F;
}
// https://graphics.stanford.edu/~seander/bithacks.html#MaskedMerge
// a ^ ((a ^ b) & mask) = r = (a & ~mask) | (b & mask)
static inline void maschine_jam_set_knob_nibble(u8 *data, uint8_t offset, uint8_t value){
	data[offset / 2] = data[offset / 2] ^ ((data[offset / 2] ^ value) & (0x0F << ((offset % 2) * 4)));
}
static inline uint8_t maschine_jam_get_button_bit(u8 *data, uint8_t offset){
	return test_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static inline void maschine_jam_set_button_bit(u8 *data, uint8_t offset){
	set_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static inline void maschine_jam_clear_button_bit(u8 *data, uint8_t offset){
	clear_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static inline void maschine_jam_toggle_button_bit(u8 *data, uint8_t offset){
	change_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static int maschine_jam_process_report01_knobs_data(struct maschine_jam_driver_data *driver_data, u8 *data){
	int return_value = 0;
	unsigned int knob_nibble;
	uint8_t old_knob_value, new_knob_value;
	struct maschine_jam_midi_config *knob_config;

	//printk(KERN_ALERT "report - %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);

	for (knob_nibble = 0; knob_nibble < MASCHINE_JAM_NUMBER_KNOBS; knob_nibble++){
		old_knob_value = maschine_jam_get_knob_nibble(driver_data->hid_report01_data_knobs, knob_nibble);
		new_knob_value = maschine_jam_get_knob_nibble(data, knob_nibble);
		if (old_knob_value != new_knob_value){
			//printk(KERN_ALERT "knob_nibble: %d, old value: %d, new value: %d", knob_nibble, old_knob_value, new_knob_value);
			knob_config = &driver_data->midi_in_knob_configs[knob_nibble];
			maschine_jam_set_knob_nibble(driver_data->hid_report01_data_knobs, knob_nibble, new_knob_value);
			return_value = maschine_jam_write_midi_event(
				driver_data,
				knob_config->type,
				knob_config->channel,
				knob_config->key,
				new_knob_value == (old_knob_value+1) % 0x10 ? 1 : 0
			);
		}
	}
	return return_value;
}
static int maschine_jam_process_report01_buttons_data(struct maschine_jam_driver_data *driver_data, u8 *data){
	int return_value = 0;
	unsigned int button_bit;
	uint8_t old_button_value, new_button_value;
	struct maschine_jam_midi_config *button_config;
	unsigned char shift_message[] = { 0xf0, 0x00, 0x21, 0x09, 0x15, 0x00, 0x4d, 0x50, 0x00, 0x01, 0x4d, 0x00, 0xf7 };

	//printk(KERN_ALERT "report - %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);

	for (button_bit = 0; button_bit < MASCHINE_JAM_NUMBER_BUTTONS; button_bit++){
		old_button_value = maschine_jam_get_button_bit(driver_data->hid_report01_data_buttons, button_bit);
		new_button_value = maschine_jam_get_button_bit(data, button_bit);
		if (old_button_value != new_button_value){
			//printk(KERN_ALERT "button_bit: %d, old value: %d, new value: %d", button_bit, old_button_value, new_button_value);
			button_config = &driver_data->midi_in_button_configs[button_bit];
			maschine_jam_toggle_button_bit(driver_data->hid_report01_data_buttons, button_bit);
			if (button_bit == 105){
				shift_message[11] |= new_button_value;
				return_value = maschine_jam_write_sysex_event(
					driver_data,
					shift_message,
					sizeof(shift_message)
				);
			}
			return_value |= maschine_jam_write_midi_event(
				driver_data,
				button_config->type,
				button_config->channel,
				button_config->key,
				button_config->value_max * new_button_value
			);
		}
	}
	return return_value;
}
struct maschine_jam_smartstrip {
	uint16_t timestamp;
	uint16_t touch_value[MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGERS];
};
static inline struct maschine_jam_smartstrip maschine_jam_get_smartstrip(u8 *data, uint8_t index){
	u8 *smartstrip_data = &data[index * MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_BITS / 8];
	return (struct maschine_jam_smartstrip){
		.timestamp = smartstrip_data[0] + (smartstrip_data[1] << 8),
		.touch_value[0] = smartstrip_data[2] + ((smartstrip_data[3] & 0x03) << 8),
		.touch_value[1] = smartstrip_data[4] + ((smartstrip_data[5] & 0x03) << 8)
	};
}
static inline void maschine_jam_set_smartstrip(u8 *data, uint8_t index, struct maschine_jam_smartstrip smartstrip){
	u8 *smartstrip_data = &data[index * MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_BITS / 8];
	smartstrip_data[0] = smartstrip.timestamp & 0xFF;
	smartstrip_data[1] = smartstrip.timestamp >> 8;
	smartstrip_data[2] = smartstrip.touch_value[0] & 0xFF;
	smartstrip_data[3] = (smartstrip.touch_value[0] >> 8) & 0x03;
	smartstrip_data[4] = smartstrip.touch_value[1] & 0xFF;
	smartstrip_data[5] = (smartstrip.touch_value[1] >> 8) & 0x03;
}
static inline void maschine_jam_set_smartstrip_touch_value(u8 *data, uint8_t index, uint8_t touch_index, uint16_t value){
	u8 *smartstrip_data = &data[index * MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_BITS / 8];
	u8 *smartstrip_value_data = &smartstrip_data[(MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_TIMESTAMP_BITS + (MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_TOUCH_VALUE_BITS * touch_index)) / 8];
	smartstrip_value_data[0] = value & 0xFF;
	smartstrip_value_data[1] = value >> 8;
}
static int maschine_jam_process_report02_smartstrips_data(struct maschine_jam_driver_data *driver_data, u8 *data){
	int return_value = 0;
	unsigned int smartstrip_index, touch_index, smartstrip_data_is_dirty;
	struct maschine_jam_smartstrip old_smartstrip, new_smartstrip;
	struct maschine_jam_midi_config *smartstrip_config;

	//printk(KERN_ALERT "report - %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
	//data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15],
	//data[16], data[17], data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26], data[27], data[28], data[29], data[30], data[31],
	//data[32], data[33], data[34], data[35], data[36], data[37], data[38], data[39], data[40], data[41], data[42], data[43], data[44], data[45], data[46], data[47]);

	for (smartstrip_index = 0; smartstrip_index < MASCHINE_JAM_NUMBER_SMARTSTRIPS; smartstrip_index++){
		old_smartstrip = maschine_jam_get_smartstrip(driver_data->hid_report02_data_smartstrips, smartstrip_index);
		new_smartstrip = maschine_jam_get_smartstrip(data, smartstrip_index);
		smartstrip_data_is_dirty = (old_smartstrip.timestamp != new_smartstrip.timestamp);
		for (touch_index = 0; touch_index < MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGERS; touch_index++){
			if (old_smartstrip.touch_value[touch_index] != new_smartstrip.touch_value[touch_index]){
				//printk(KERN_ALERT "old_smartstrip: smartstrip_index: %d, timestamp: %04X, touch_value[0] %04X, touch_value[1] %04X", smartstrip_index, old_smartstrip.timestamp, old_smartstrip.touch_value[0], old_smartstrip.touch_value[1]);
				//printk(KERN_ALERT "new_smartstrip: smartstrip_index: %d, timestamp: %04X, touch_value[0] %04X, touch_value[1] %04X", smartstrip_index, new_smartstrip.timestamp, new_smartstrip.touch_value[0], new_smartstrip.touch_value[1]);
				smartstrip_data_is_dirty = 1;
				if (old_smartstrip.touch_value[touch_index] == 0 || new_smartstrip.touch_value[touch_index] == 0){
					smartstrip_config = &driver_data->midi_in_smartstrip_configs[smartstrip_index][touch_index][MJ_SMARTSTRIP_FINGER_MODE_TOUCH];
					maschine_jam_write_midi_event(
						driver_data,
						smartstrip_config->type,
						smartstrip_config->channel,
						smartstrip_config->key,
						new_smartstrip.touch_value[touch_index] ? 127 : 0
					);
				}
				if (new_smartstrip.touch_value[touch_index] != 0){
					smartstrip_config = &driver_data->midi_in_smartstrip_configs[smartstrip_index][touch_index][MJ_SMARTSTRIP_FINGER_MODE_SLIDE];
					maschine_jam_write_midi_event(
						driver_data,
						smartstrip_config->type,
						smartstrip_config->channel,
						smartstrip_config->key,
						new_smartstrip.touch_value[touch_index] >> 3
					);
				}
			}
		}
		if (smartstrip_data_is_dirty){
			// !!! Put inside lock
			maschine_jam_set_smartstrip(driver_data->hid_report02_data_smartstrips, smartstrip_index, new_smartstrip);
		}
	}
	return return_value;
}

static int maschine_jam_raw_event(struct hid_device *mj_hid_device, struct hid_report *report, u8 *data, int size){
	int return_value = 0;
	struct maschine_jam_driver_data *driver_data;

	if (mj_hid_device != NULL && report != NULL && data != NULL && report->id == data[0]){
		driver_data = hid_get_drvdata(mj_hid_device);
		if (report->id == 0x01 && size == MASCHINE_JAM_HID_REPORT_01_BYTES){
			// !!! Validate report
			// smartstrip_index < smartstrips_hid_field->report_count == MASCHINE_JAM_NUMBER_SMARTSTRIPS * MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGERS
			maschine_jam_process_report01_knobs_data(driver_data, &data[MASCHINE_JAM_HID_REPORT_ID_BYTES]);
			maschine_jam_process_report01_buttons_data(driver_data, &data[MASCHINE_JAM_HID_REPORT_ID_BYTES + MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES]);
		} else if (report->id == 0x02 && size == MASCHINE_JAM_HID_REPORT_02_BYTES){
			// !!! Validate report
			// smartstrip_index < smartstrips_hid_field->report_count == MASCHINE_JAM_NUMBER_SMARTSTRIPS * MASCHINE_JAM_NUMBER_SMARTSTRIP_FINGERS
			maschine_jam_process_report02_smartstrips_data(driver_data, &data[MASCHINE_JAM_HID_REPORT_ID_BYTES]);
		} else {
			printk(KERN_ALERT "maschine_jam_raw_event() - error - report id is unknown or bad data size\n");
		}
	} else {
		printk(KERN_ALERT "maschine_jam_raw_event() - error - bad parameters\n");
	}
	return return_value;
}

enum maschine_jam_io_attribute_type {
	IO_ATTRIBUTE_INPUT_KNOB,
	IO_ATTRIBUTE_INPUT_BUTTON,
	IO_ATTRIBUTE_INPUT_SMARTSTRIP,
	IO_ATTRIBUTE_OUTPUT_BUTTON_LED,
	IO_ATTRIBUTE_OUTPUT_PAD_LED,
	IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED,
	IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_MODE,
	IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_COLOR,
	IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_VALUE,
};
struct maschine_jam_io_attribute {
	struct kobj_attribute type_attribute;
	struct kobj_attribute channel_attribute;
	struct kobj_attribute key_attribute;
	struct kobj_attribute status_attribute;
	enum maschine_jam_io_attribute_type io_attribute_type;
	uint8_t io_index;
	uint8_t smartstrip_finger;
	uint8_t smartstrip_finger_mode;
};
static ssize_t maschine_jam_inputs_type_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, type_attribute);
	enum maschine_jam_midi_type midi_type;

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		midi_type = driver_data->midi_in_knob_configs[io_attribute->io_index].type;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		midi_type = driver_data->midi_in_button_configs[io_attribute->io_index].type;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		midi_type = driver_data->midi_in_smartstrip_configs[io_attribute->io_index][io_attribute->smartstrip_finger][io_attribute->smartstrip_finger_mode].type;
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
	switch (midi_type){
		case MJ_MIDI_TYPE_NOTE:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_NOTE_STRING);
		case MJ_MIDI_TYPE_AFTERTOUCH:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_AFTERTOUCH_STRING);
		case MJ_MIDI_TYPE_CONTROL_CHANGE:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_CONTROL_CHANGE_STRING);
		default:
			return scnprintf(buf, PAGE_SIZE, "unknown midi type\n");
	}
}
static ssize_t maschine_jam_inputs_type_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, type_attribute);
	enum maschine_jam_midi_type midi_type;

	if (strncmp(buf, MJ_MIDI_TYPE_NOTE_STRING, sizeof(MJ_MIDI_TYPE_NOTE_STRING)) == 0){
		midi_type = MJ_MIDI_TYPE_NOTE;
	} else if (strncmp(buf, MJ_MIDI_TYPE_AFTERTOUCH_STRING, sizeof(MJ_MIDI_TYPE_AFTERTOUCH_STRING)) == 0){
		midi_type = MJ_MIDI_TYPE_AFTERTOUCH;
	} else if (strncmp(buf, MJ_MIDI_TYPE_CONTROL_CHANGE_STRING, sizeof(MJ_MIDI_TYPE_CONTROL_CHANGE_STRING)) == 0){
		midi_type = MJ_MIDI_TYPE_CONTROL_CHANGE;
	} else {
		printk(KERN_ALERT "maschine_jam_inputs_type_store - invalid type\n");
		return count;
	}
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		driver_data->midi_in_knob_configs[io_attribute->io_index].type = midi_type;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		driver_data->midi_in_button_configs[io_attribute->io_index].type = midi_type;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		driver_data->midi_in_smartstrip_configs[io_attribute->io_index][io_attribute->smartstrip_finger][io_attribute->smartstrip_finger_mode].type = midi_type;
	}
	return count;
}
static ssize_t maschine_jam_inputs_channel_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, channel_attribute);

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		return scnprintf(buf, PAGE_SIZE, "%d\n", driver_data->midi_in_knob_configs[io_attribute->io_index].channel);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		return scnprintf(buf, PAGE_SIZE, "%d\n", driver_data->midi_in_button_configs[io_attribute->io_index].channel);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		return scnprintf(buf, PAGE_SIZE, "%d\n", driver_data->midi_in_smartstrip_configs[io_attribute->io_index][io_attribute->smartstrip_finger][io_attribute->smartstrip_finger_mode].channel);
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
}
static ssize_t maschine_jam_inputs_channel_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	unsigned int store_value;
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, channel_attribute);

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		driver_data->midi_in_knob_configs[io_attribute->io_index].channel = store_value & 0xF;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		driver_data->midi_in_button_configs[io_attribute->io_index].channel = store_value & 0xF;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		driver_data->midi_in_smartstrip_configs[io_attribute->io_index][io_attribute->smartstrip_finger][io_attribute->smartstrip_finger_mode].channel = store_value & 0xF;
	}
	return count;
}
static ssize_t maschine_jam_inputs_key_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, key_attribute);

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		return scnprintf(buf, PAGE_SIZE, "%d\n", driver_data->midi_in_knob_configs[io_attribute->io_index].key);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		return scnprintf(buf, PAGE_SIZE, "%d\n", driver_data->midi_in_button_configs[io_attribute->io_index].key);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		return scnprintf(buf, PAGE_SIZE, "%d\n", driver_data->midi_in_smartstrip_configs[io_attribute->io_index][io_attribute->smartstrip_finger][io_attribute->smartstrip_finger_mode].key);
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
}
static ssize_t maschine_jam_inputs_key_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	unsigned int store_value;
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, key_attribute);

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		driver_data->midi_in_knob_configs[io_attribute->io_index].key = store_value & 0x7F;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		driver_data->midi_in_button_configs[io_attribute->io_index].key = store_value & 0x7F;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		driver_data->midi_in_smartstrip_configs[io_attribute->io_index][io_attribute->smartstrip_finger][io_attribute->smartstrip_finger_mode].key = store_value & 0x7F;
	}
	return count;
}
static ssize_t maschine_jam_inputs_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, status_attribute);

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_knob_nibble(driver_data->hid_report01_data_knobs, io_attribute->io_index));
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_button_bit(driver_data->hid_report01_data_buttons, io_attribute->io_index));
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_smartstrip(driver_data->hid_report02_data_smartstrips, io_attribute->io_index/2).touch_value[io_attribute->io_index % 2]);
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
}
static ssize_t maschine_jam_inputs_status_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	unsigned int store_value;
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, status_attribute);

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		maschine_jam_set_knob_nibble(driver_data->hid_report01_data_knobs, io_attribute->io_index, store_value & 0x0F);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		if (store_value == 0){
			maschine_jam_clear_button_bit(driver_data->hid_report01_data_buttons, io_attribute->io_index);
		} else {
			maschine_jam_set_button_bit(driver_data->hid_report01_data_buttons, io_attribute->io_index);
		}
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		maschine_jam_set_smartstrip_touch_value(driver_data->hid_report02_data_smartstrips, io_attribute->io_index/2, io_attribute->io_index%2, store_value & 0x03FF);
	}
	return count;
}
#define MJ_INPUTS_KNOB_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_inputs_knob_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_type_show, \
			.store = maschine_jam_inputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_channel_show, \
			.store = maschine_jam_inputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_key_show, \
			.store = maschine_jam_inputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_status_show, \
			.store = maschine_jam_inputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_INPUT_KNOB, \
		.io_index = _index, \
		.smartstrip_finger = 0, \
		.smartstrip_finger_mode = 0, \
	}; \
	static struct attribute *maschine_jam_inputs_knob_ ## _name ## _attributes[] = { \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_inputs_knob_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_inputs_knob_ ## _name ## _attributes, \
	}
MJ_INPUTS_KNOB_ATTRIBUTE_GROUP(encoder, 0);
//MJ_INPUTS_KNOB_ATTRIBUTE_GROUP(unknown_knob, 1);
static const struct attribute_group *maschine_jam_inputs_knobs_groups[] = {
	&maschine_jam_inputs_knob_encoder_group,
	//&maschine_jam_inputs_knob_unknown_knob_group,
	NULL
};
#define MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_inputs_button_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_type_show, \
			.store = maschine_jam_inputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_channel_show, \
			.store = maschine_jam_inputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_key_show, \
			.store = maschine_jam_inputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_status_show, \
			.store = maschine_jam_inputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_INPUT_BUTTON, \
		.io_index = _index, \
		.smartstrip_finger = 0, \
		.smartstrip_finger_mode = 0, \
	}; \
	static struct attribute *maschine_jam_inputs_button_ ## _name ## _attributes[] = { \
		&maschine_jam_inputs_button_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_inputs_button_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_inputs_button_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_inputs_button_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_inputs_button_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_inputs_button_ ## _name ## _attributes, \
	}
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(song, 0);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_1, 1);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_2, 2);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_3, 3);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_4, 4);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_5, 5);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_6, 6);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_7, 7);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_8, 8);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(step, 9);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(pad_mode, 10);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(clear, 11);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(duplicate, 12);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_up, 13);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_left, 14);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_right, 15);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_down, 16);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(note_repeat, 17);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x1, 18);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x2, 19);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x3, 20);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x4, 21);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x5, 22);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x6, 23);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x7, 24);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x8, 25);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x1, 26);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x2, 27);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x3, 28);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x4, 29);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x5, 30);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x6, 31);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x7, 32);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x8, 33);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x1, 34);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x2, 35);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x3, 36);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x4, 37);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x5, 38);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x6, 39);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x7, 40);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x8, 41);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x1, 42);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x2, 43);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x3, 44);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x4, 45);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x5, 46);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x6, 47);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x7, 48);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x8, 49);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x1, 50);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x2, 51);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x3, 52);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x4, 53);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x5, 54);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x6, 55);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x7, 56);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x8, 57);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x1, 58);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x2, 59);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x3, 60);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x4, 61);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x5, 62);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x6, 63);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x7, 64);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x8, 65);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x1, 66);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x2, 67);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x3, 68);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x4, 69);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x5, 70);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x6, 71);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x7, 72);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x8, 73);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x1, 74);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x2, 75);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x3, 76);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x4, 77);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x5, 78);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x6, 79);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x7, 80);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x8, 81);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_a, 82);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_b, 83);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_c, 84);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_d, 85);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_e, 86);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_f, 87);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_g, 88);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_h, 89);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(mst, 90);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(grp, 91);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(in_1, 92);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(cue, 93);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(browse, 94);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(macro, 95);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(level, 96);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(aux, 97);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(control, 98);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(auto, 99);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(perform, 100);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(notes, 101);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(lock, 102);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(tune, 103);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(swing, 104);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(shift, 105);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(play, 106);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(rec, 107);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(page_left, 108);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(page_right, 109);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(tempo, 110);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(grid, 111);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(solo, 112);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(mute, 113);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(select, 114);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(encoder_touch, 115);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(encoder_push, 116);
//MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(unknown_117, 117); // footswitch/line-in?
//MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(unknown_118, 118);
//MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(unknown_119, 119);

static const struct attribute_group *maschine_jam_inputs_buttons_groups[] = {
	&maschine_jam_inputs_button_song_group,
	&maschine_jam_inputs_button_scene_1_group,
	&maschine_jam_inputs_button_scene_2_group,
	&maschine_jam_inputs_button_scene_3_group,
	&maschine_jam_inputs_button_scene_4_group,
	&maschine_jam_inputs_button_scene_5_group,
	&maschine_jam_inputs_button_scene_6_group,
	&maschine_jam_inputs_button_scene_7_group,
	&maschine_jam_inputs_button_scene_8_group,
	&maschine_jam_inputs_button_step_group,
	&maschine_jam_inputs_button_pad_mode_group,
	&maschine_jam_inputs_button_clear_group,
	&maschine_jam_inputs_button_duplicate_group,
	&maschine_jam_inputs_button_navigate_up_group,
	&maschine_jam_inputs_button_navigate_left_group,
	&maschine_jam_inputs_button_navigate_right_group,
	&maschine_jam_inputs_button_navigate_down_group,
	&maschine_jam_inputs_button_note_repeat_group,
	&maschine_jam_inputs_button_matrix_1x1_group,
	&maschine_jam_inputs_button_matrix_1x2_group,
	&maschine_jam_inputs_button_matrix_1x3_group,
	&maschine_jam_inputs_button_matrix_1x4_group,
	&maschine_jam_inputs_button_matrix_1x5_group,
	&maschine_jam_inputs_button_matrix_1x6_group,
	&maschine_jam_inputs_button_matrix_1x7_group,
	&maschine_jam_inputs_button_matrix_1x8_group,
	&maschine_jam_inputs_button_matrix_2x1_group,
	&maschine_jam_inputs_button_matrix_2x2_group,
	&maschine_jam_inputs_button_matrix_2x3_group,
	&maschine_jam_inputs_button_matrix_2x4_group,
	&maschine_jam_inputs_button_matrix_2x5_group,
	&maschine_jam_inputs_button_matrix_2x6_group,
	&maschine_jam_inputs_button_matrix_2x7_group,
	&maschine_jam_inputs_button_matrix_2x8_group,
	&maschine_jam_inputs_button_matrix_3x1_group,
	&maschine_jam_inputs_button_matrix_3x2_group,
	&maschine_jam_inputs_button_matrix_3x3_group,
	&maschine_jam_inputs_button_matrix_3x4_group,
	&maschine_jam_inputs_button_matrix_3x5_group,
	&maschine_jam_inputs_button_matrix_3x6_group,
	&maschine_jam_inputs_button_matrix_3x7_group,
	&maschine_jam_inputs_button_matrix_3x8_group,
	&maschine_jam_inputs_button_matrix_4x1_group,
	&maschine_jam_inputs_button_matrix_4x2_group,
	&maschine_jam_inputs_button_matrix_4x3_group,
	&maschine_jam_inputs_button_matrix_4x4_group,
	&maschine_jam_inputs_button_matrix_4x5_group,
	&maschine_jam_inputs_button_matrix_4x6_group,
	&maschine_jam_inputs_button_matrix_4x7_group,
	&maschine_jam_inputs_button_matrix_4x8_group,
	&maschine_jam_inputs_button_matrix_5x1_group,
	&maschine_jam_inputs_button_matrix_5x2_group,
	&maschine_jam_inputs_button_matrix_5x3_group,
	&maschine_jam_inputs_button_matrix_5x4_group,
	&maschine_jam_inputs_button_matrix_5x5_group,
	&maschine_jam_inputs_button_matrix_5x6_group,
	&maschine_jam_inputs_button_matrix_5x7_group,
	&maschine_jam_inputs_button_matrix_5x8_group,
	&maschine_jam_inputs_button_matrix_6x1_group,
	&maschine_jam_inputs_button_matrix_6x2_group,
	&maschine_jam_inputs_button_matrix_6x3_group,
	&maschine_jam_inputs_button_matrix_6x4_group,
	&maschine_jam_inputs_button_matrix_6x5_group,
	&maschine_jam_inputs_button_matrix_6x6_group,
	&maschine_jam_inputs_button_matrix_6x7_group,
	&maschine_jam_inputs_button_matrix_6x8_group,
	&maschine_jam_inputs_button_matrix_7x1_group,
	&maschine_jam_inputs_button_matrix_7x2_group,
	&maschine_jam_inputs_button_matrix_7x3_group,
	&maschine_jam_inputs_button_matrix_7x4_group,
	&maschine_jam_inputs_button_matrix_7x5_group,
	&maschine_jam_inputs_button_matrix_7x6_group,
	&maschine_jam_inputs_button_matrix_7x7_group,
	&maschine_jam_inputs_button_matrix_7x8_group,
	&maschine_jam_inputs_button_matrix_8x1_group,
	&maschine_jam_inputs_button_matrix_8x2_group,
	&maschine_jam_inputs_button_matrix_8x3_group,
	&maschine_jam_inputs_button_matrix_8x4_group,
	&maschine_jam_inputs_button_matrix_8x5_group,
	&maschine_jam_inputs_button_matrix_8x6_group,
	&maschine_jam_inputs_button_matrix_8x7_group,
	&maschine_jam_inputs_button_matrix_8x8_group,
	&maschine_jam_inputs_button_group_a_group,
	&maschine_jam_inputs_button_group_b_group,
	&maschine_jam_inputs_button_group_c_group,
	&maschine_jam_inputs_button_group_d_group,
	&maschine_jam_inputs_button_group_e_group,
	&maschine_jam_inputs_button_group_f_group,
	&maschine_jam_inputs_button_group_g_group,
	&maschine_jam_inputs_button_group_h_group,
	&maschine_jam_inputs_button_mst_group,
	&maschine_jam_inputs_button_grp_group,
	&maschine_jam_inputs_button_in_1_group,
	&maschine_jam_inputs_button_cue_group,
	&maschine_jam_inputs_button_browse_group,
	&maschine_jam_inputs_button_macro_group,
	&maschine_jam_inputs_button_level_group,
	&maschine_jam_inputs_button_aux_group,
	&maschine_jam_inputs_button_control_group,
	&maschine_jam_inputs_button_auto_group,
	&maschine_jam_inputs_button_perform_group,
	&maschine_jam_inputs_button_notes_group,
	&maschine_jam_inputs_button_lock_group,
	&maschine_jam_inputs_button_tune_group,
	&maschine_jam_inputs_button_swing_group,
	&maschine_jam_inputs_button_shift_group,
	&maschine_jam_inputs_button_play_group,
	&maschine_jam_inputs_button_rec_group,
	&maschine_jam_inputs_button_page_left_group,
	&maschine_jam_inputs_button_page_right_group,
	&maschine_jam_inputs_button_tempo_group,
	&maschine_jam_inputs_button_grid_group,
	&maschine_jam_inputs_button_solo_group,
	&maschine_jam_inputs_button_mute_group,
	&maschine_jam_inputs_button_select_group,
	&maschine_jam_inputs_button_encoder_touch_group,
	&maschine_jam_inputs_button_encoder_push_group,
	//&maschine_jam_inputs_button_unknown_117_group,
	//&maschine_jam_inputs_button_unknown_118_group,
	//&maschine_jam_inputs_button_unknown_119_group,
	NULL
};
#define MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(_name, _index, _smartstrip_finger, _smartstrip_finger_mode) \
	struct maschine_jam_io_attribute maschine_jam_inputs_smartstrip_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_type_show, \
			.store = maschine_jam_inputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_channel_show, \
			.store = maschine_jam_inputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_key_show, \
			.store = maschine_jam_inputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_status_show, \
			.store = maschine_jam_inputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_INPUT_SMARTSTRIP, \
		.io_index = _index, \
		.smartstrip_finger = _smartstrip_finger, \
		.smartstrip_finger_mode = _smartstrip_finger_mode, \
	}; \
	static struct attribute *maschine_jam_inputs_smartstrip_ ## _name ## _attributes[] = { \
		&maschine_jam_inputs_smartstrip_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_inputs_smartstrip_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_inputs_smartstrip_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_inputs_smartstrip_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_inputs_smartstrip_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_inputs_smartstrip_ ## _name ## _attributes, \
	}
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(1AT, 0, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(1AS, 0, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(1BT, 0, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(1BS, 0, 1, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(2AT, 1, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(2AS, 1, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(2BT, 1, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(2BS, 1, 1, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(3AT, 2, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(3AS, 2, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(3BT, 2, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(3BS, 2, 1, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(4AT, 3, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(4AS, 3, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(4BT, 3, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(4BS, 3, 1, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(5AT, 4, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(5AS, 4, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(5BT, 4, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(5BS, 4, 1, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(6AT, 5, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(6AS, 5, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(6BT, 5, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(6BS, 5, 1, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(7AT, 6, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(7AS, 6, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(7BT, 6, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(7BS, 6, 1, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(8AT, 7, 0, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(8AS, 7, 0, 1);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(8BT, 7, 1, 0);
MJ_INPUTS_SMARTSTRIP_ATTRIBUTE_GROUP(8BS, 7, 1, 1);
static const struct attribute_group *maschine_jam_inputs_smartstrips_groups[] = {
	&maschine_jam_inputs_smartstrip_1AT_group,
	&maschine_jam_inputs_smartstrip_1AS_group,
	&maschine_jam_inputs_smartstrip_1BT_group,
	&maschine_jam_inputs_smartstrip_1BS_group,
	&maschine_jam_inputs_smartstrip_2AT_group,
	&maschine_jam_inputs_smartstrip_2AS_group,
	&maschine_jam_inputs_smartstrip_2BT_group,
	&maschine_jam_inputs_smartstrip_2BS_group,
	&maschine_jam_inputs_smartstrip_3AT_group,
	&maschine_jam_inputs_smartstrip_3AS_group,
	&maschine_jam_inputs_smartstrip_3BT_group,
	&maschine_jam_inputs_smartstrip_3BS_group,
	&maschine_jam_inputs_smartstrip_4AT_group,
	&maschine_jam_inputs_smartstrip_4AS_group,
	&maschine_jam_inputs_smartstrip_4BT_group,
	&maschine_jam_inputs_smartstrip_4BS_group,
	&maschine_jam_inputs_smartstrip_5AT_group,
	&maschine_jam_inputs_smartstrip_5AS_group,
	&maschine_jam_inputs_smartstrip_5BT_group,
	&maschine_jam_inputs_smartstrip_5BS_group,
	&maschine_jam_inputs_smartstrip_6AT_group,
	&maschine_jam_inputs_smartstrip_6AS_group,
	&maschine_jam_inputs_smartstrip_6BT_group,
	&maschine_jam_inputs_smartstrip_6BS_group,
	&maschine_jam_inputs_smartstrip_7AT_group,
	&maschine_jam_inputs_smartstrip_7AS_group,
	&maschine_jam_inputs_smartstrip_7BT_group,
	&maschine_jam_inputs_smartstrip_7BS_group,
	&maschine_jam_inputs_smartstrip_8AT_group,
	&maschine_jam_inputs_smartstrip_8AS_group,
	&maschine_jam_inputs_smartstrip_8BT_group,
	&maschine_jam_inputs_smartstrip_8BS_group,
	NULL
};

static ssize_t maschine_jam_outputs_type_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_outputs_type_dir = kobj;
	struct kobject *maschine_jam_outputs_dir = maschine_jam_outputs_type_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_outputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, type_attribute);
	struct maschine_jam_output_node* output_node;
	struct snd_seq_event midi_event;

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_BUTTON_LED){
		output_node = &driver_data->midi_out_button_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_PAD_LED){
		output_node = &driver_data->midi_out_pad_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED) {
		output_node = &driver_data->midi_out_smartstrip_led_nodes[io_attribute->io_index];
	} else {
		return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_type_show: invalid io_attribute_type\n");
	}
	if (maschine_jam_output_mapping_get_midi_info(output_node, &midi_event) < 0){
		return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_type_show: unable to get midi info\n");
	}
	switch (midi_event.type){
		case SNDRV_SEQ_EVENT_NOTE:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_NOTE_STRING);
		case SNDRV_SEQ_EVENT_CONTROLLER:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_CONTROL_CHANGE_STRING);
		default:
			return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_type_show: unknown midi type\n");
	}
}
static ssize_t maschine_jam_outputs_type_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	struct kobject *maschine_jam_outputs_type_dir = kobj;
	struct kobject *maschine_jam_outputs_dir = maschine_jam_outputs_type_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_outputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, type_attribute);
	snd_seq_event_type_t input_type;
	struct maschine_jam_output_node* output_node;
	struct snd_seq_event midi_event;
	uint8_t channel, key;

	if (strncmp(buf, MJ_MIDI_TYPE_NOTE_STRING, sizeof(MJ_MIDI_TYPE_NOTE_STRING)) == 0){
		input_type = SNDRV_SEQ_EVENT_NOTE;
	} else if (strncmp(buf, MJ_MIDI_TYPE_CONTROL_CHANGE_STRING, sizeof(MJ_MIDI_TYPE_CONTROL_CHANGE_STRING)) == 0){
		input_type = SNDRV_SEQ_EVENT_CONTROLLER;
	} else {
		printk(KERN_ALERT "maschine_jam_outputs_type_store - invalid input_type\n");
		return count;
	}
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_BUTTON_LED){
		output_node = &driver_data->midi_out_button_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_PAD_LED){
		output_node = &driver_data->midi_out_pad_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED) {
		output_node = &driver_data->midi_out_smartstrip_led_nodes[io_attribute->io_index];
	} else {
		printk(KERN_ALERT "maschine_jam_outputs_channel_store: invalid io_attribute_type\n");
		return count;
	}
	spin_lock(&driver_data->midi_out_mapping_lock);
	if (maschine_jam_output_mapping_get_midi_info(output_node, &midi_event) < 0){
		printk(KERN_ALERT "maschine_jam_outputs_channel_store: unable to get midi info\n");
	} else {
		if (midi_event.type != input_type){
			channel = maschine_jam_snd_seq_event_get_channel(&midi_event);
			key = maschine_jam_snd_seq_event_get_key(&midi_event);
			midi_event.type = input_type;
			maschine_jam_snd_seq_event_set_channel(&midi_event, channel);
			maschine_jam_snd_seq_event_set_key(&midi_event, key);
			maschine_jam_output_mapping_set_midi_info(driver_data, output_node, &midi_event);
		}
	}
	spin_unlock(&driver_data->midi_out_mapping_lock);
	return count;
}
static ssize_t maschine_jam_outputs_channel_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_outputs_type_dir = kobj;
	struct kobject *maschine_jam_outputs_dir = maschine_jam_outputs_type_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_outputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, channel_attribute);
	struct maschine_jam_output_node* output_node;
	struct snd_seq_event midi_event;

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_BUTTON_LED){
		output_node = &driver_data->midi_out_button_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_PAD_LED){
		output_node = &driver_data->midi_out_pad_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED) {
		output_node = &driver_data->midi_out_smartstrip_led_nodes[io_attribute->io_index];
	} else {
		return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_channel_show: invalid io_attribute_type\n");
	}
	if (maschine_jam_output_mapping_get_midi_info(output_node, &midi_event) < 0){
		return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_channel_show: unable to get midi info\n");
	}
	return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_snd_seq_event_get_channel(&midi_event));
}
static ssize_t maschine_jam_outputs_channel_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	struct kobject *maschine_jam_outputs_type_dir = kobj;
	struct kobject *maschine_jam_outputs_dir = maschine_jam_outputs_type_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_outputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, channel_attribute);
	unsigned int store_value;
	struct maschine_jam_output_node* output_node;
	struct snd_seq_event midi_event;
	uint8_t channel;

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_BUTTON_LED){
		output_node = &driver_data->midi_out_button_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_PAD_LED){
		output_node = &driver_data->midi_out_pad_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED) {
		output_node = &driver_data->midi_out_smartstrip_led_nodes[io_attribute->io_index];
	} else {
		printk(KERN_ALERT "maschine_jam_outputs_channel_store: invalid io_attribute_type\n");
		return count;
	}
	spin_lock(&driver_data->midi_out_mapping_lock);
	if (maschine_jam_output_mapping_get_midi_info(output_node, &midi_event) < 0){
		printk(KERN_ALERT "maschine_jam_outputs_channel_store: unable to get midi info\n");
	} else {
		channel = maschine_jam_snd_seq_event_get_channel(&midi_event);
		if (channel >= 0 && channel != store_value){
			maschine_jam_snd_seq_event_set_channel(&midi_event, store_value);
			maschine_jam_output_mapping_set_midi_info(driver_data, output_node, &midi_event);
		}
	}
	spin_unlock(&driver_data->midi_out_mapping_lock);
	return count;
}
static ssize_t maschine_jam_outputs_key_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_outputs_type_dir = kobj;
	struct kobject *maschine_jam_outputs_dir = maschine_jam_outputs_type_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_outputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, key_attribute);
	struct maschine_jam_output_node* output_node;
	struct snd_seq_event midi_event;

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_BUTTON_LED){
		output_node = &driver_data->midi_out_button_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_PAD_LED){
		output_node = &driver_data->midi_out_pad_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED) {
		output_node = &driver_data->midi_out_smartstrip_led_nodes[io_attribute->io_index];
	} else {
		return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_key_show: invalid io_attribute_type\n");
	}
	if (maschine_jam_output_mapping_get_midi_info(output_node, &midi_event) < 0){
		return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_key_show: unable to get midi info\n");
	}
	return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_snd_seq_event_get_key(&midi_event));
}
static ssize_t maschine_jam_outputs_key_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	struct kobject *maschine_jam_outputs_type_dir = kobj;
	struct kobject *maschine_jam_outputs_dir = maschine_jam_outputs_type_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_outputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, key_attribute);
	unsigned int store_value;
	struct maschine_jam_output_node* output_node;
	struct snd_seq_event midi_event;
	uint8_t key;

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_BUTTON_LED){
		output_node = &driver_data->midi_out_button_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_PAD_LED){
		output_node = &driver_data->midi_out_pad_led_nodes[io_attribute->io_index];
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED) {
		output_node = &driver_data->midi_out_smartstrip_led_nodes[io_attribute->io_index];
	} else {
		printk(KERN_ALERT "maschine_jam_outputs_key_store: invalid io_attribute_type\n");
		return count;
	}
	spin_lock(&driver_data->midi_out_mapping_lock);
	if (maschine_jam_output_mapping_get_midi_info(output_node, &midi_event) < 0){
		printk(KERN_ALERT "maschine_jam_outputs_key_store: unable to get midi info\n");
	} else {
		key = maschine_jam_snd_seq_event_get_key(&midi_event);
		if (key >= 0 && key != store_value){
			maschine_jam_snd_seq_event_set_key(&midi_event, store_value);
			maschine_jam_output_mapping_set_midi_info(driver_data, output_node, &midi_event);
		}
	}
	spin_unlock(&driver_data->midi_out_mapping_lock);
	return count;
}
static ssize_t maschine_jam_outputs_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	//struct kobject *maschine_jam_inputs_button_dir = kobj;
	//struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	//struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	//struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	//struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	//struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	//struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, status_attribute);

	//if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		//return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_knob_nibble(driver_data->hid_report01_data_knobs, io_attribute->io_index));
	//} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		//return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_button_bit(driver_data->hid_report01_data_buttons, io_attribute->io_index));
	//} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		//return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_smartstrip(driver_data->hid_report02_data_smartstrips, io_attribute->io_index/2).touch_value[io_attribute->io_index % 2]);
	//} else {
		//return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_status_show: unknown attribute type\n");
	//}
	return scnprintf(buf, PAGE_SIZE, "maschine_jam_outputs_status_show: unimplmented\n");
}
static ssize_t maschine_jam_outputs_status_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	//unsigned int store_value;
	//struct kobject *maschine_jam_inputs_button_dir = kobj;
	//struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	//struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	//struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	//struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	//struct maschine_jam_driver_data *driver_data = hid_get_drvdata(hdev);
	//struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, status_attribute);

	//sscanf(buf, "%u", &store_value);
	//if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_KNOB){
		//maschine_jam_set_knob_nibble(driver_data->hid_report01_data_knobs, io_attribute->io_index, store_value & 0x0F);
	//} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_BUTTON){
		//if (store_value == 0){
			//maschine_jam_clear_button_bit(driver_data->hid_report01_data_buttons, io_attribute->io_index);
		//} else {
			//maschine_jam_set_button_bit(driver_data->hid_report01_data_buttons, io_attribute->io_index);
		//}
	//} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_INPUT_SMARTSTRIP){
		//maschine_jam_set_smartstrip_touch_value(driver_data->hid_report02_data_smartstrips, io_attribute->io_index/2, io_attribute->io_index%2, store_value & 0x03FF);
	//}
	//printk(KERN_ALERT "maschine_jam_outputs_status_store - invalid type\n");
	printk(KERN_ALERT "maschine_jam_outputs_status_store - unimplemented\n");
	return count;
}
#define MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_outputs_button_led_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_type_show, \
			.store = maschine_jam_outputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_channel_show, \
			.store = maschine_jam_outputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_key_show, \
			.store = maschine_jam_outputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_status_show, \
			.store = maschine_jam_outputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_OUTPUT_BUTTON_LED, \
		.io_index = _index, \
		.smartstrip_finger = 0, \
		.smartstrip_finger_mode = 0, \
	}; \
	static struct attribute *maschine_jam_outputs_button_led_ ## _name ## _attributes[] = { \
		&maschine_jam_outputs_button_led_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_outputs_button_led_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_outputs_button_led_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_outputs_button_led_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_outputs_button_led_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_outputs_button_led_ ## _name ## _attributes, \
	}
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(song, 0);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(step, 1);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(pad_mode, 2);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(clear, 3);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(duplicate, 4);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(navigate_up, 5);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(navigate_left, 6);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(navigate_right, 7);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(navigate_down, 8);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(note_repeat, 9);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(mst, 10);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(grp, 11);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(in_1, 12);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(unknown_1, 13);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(cue, 14);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(unknown_2, 15);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(browse, 16);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(macro, 17);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level, 18);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(aux, 19);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(control, 20);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(auto, 21);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(perform, 22);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(notes, 23);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(lock, 24);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(tune, 25);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(swing, 26);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(shift, 27);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(play, 28);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(rec, 29);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(page_left, 30);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(page_right, 31);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(tempo, 32);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(grid, 33);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(solo, 34);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(mute, 35);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(select, 36);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_1, 37);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_2, 38);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_3, 39);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_4, 40);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_5, 41);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_6, 42);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_7, 43);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_left_8, 44);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_1, 45);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_2, 46);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_3, 47);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_4, 48);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_5, 49);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_6, 40);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_7, 51);
MJ_OUTPUTS_BUTTON_LED_ATTRIBUTE_GROUP(level_right_8, 52);

static const struct attribute_group *maschine_jam_outputs_button_led_groups[] = {
	&maschine_jam_outputs_button_led_song_group,
	&maschine_jam_outputs_button_led_step_group,
	&maschine_jam_outputs_button_led_pad_mode_group,
	&maschine_jam_outputs_button_led_clear_group,
	&maschine_jam_outputs_button_led_duplicate_group,
	&maschine_jam_outputs_button_led_navigate_up_group,
	&maschine_jam_outputs_button_led_navigate_left_group,
	&maschine_jam_outputs_button_led_navigate_right_group,
	&maschine_jam_outputs_button_led_navigate_down_group,
	&maschine_jam_outputs_button_led_note_repeat_group,
	&maschine_jam_outputs_button_led_mst_group,
	&maschine_jam_outputs_button_led_grp_group,
	&maschine_jam_outputs_button_led_in_1_group,
	&maschine_jam_outputs_button_led_unknown_1_group,
	&maschine_jam_outputs_button_led_cue_group,
	&maschine_jam_outputs_button_led_unknown_2_group,
	&maschine_jam_outputs_button_led_browse_group,
	&maschine_jam_outputs_button_led_macro_group,
	&maschine_jam_outputs_button_led_level_group,
	&maschine_jam_outputs_button_led_aux_group,
	&maschine_jam_outputs_button_led_control_group,
	&maschine_jam_outputs_button_led_auto_group,
	&maschine_jam_outputs_button_led_perform_group,
	&maschine_jam_outputs_button_led_notes_group,
	&maschine_jam_outputs_button_led_lock_group,
	&maschine_jam_outputs_button_led_tune_group,
	&maschine_jam_outputs_button_led_swing_group,
	&maschine_jam_outputs_button_led_shift_group,
	&maschine_jam_outputs_button_led_play_group,
	&maschine_jam_outputs_button_led_rec_group,
	&maschine_jam_outputs_button_led_page_left_group,
	&maschine_jam_outputs_button_led_page_right_group,
	&maschine_jam_outputs_button_led_tempo_group,
	&maschine_jam_outputs_button_led_grid_group,
	&maschine_jam_outputs_button_led_solo_group,
	&maschine_jam_outputs_button_led_mute_group,
	&maschine_jam_outputs_button_led_select_group,
	&maschine_jam_outputs_button_led_level_left_1_group,
	&maschine_jam_outputs_button_led_level_left_2_group,
	&maschine_jam_outputs_button_led_level_left_3_group,
	&maschine_jam_outputs_button_led_level_left_4_group,
	&maschine_jam_outputs_button_led_level_left_5_group,
	&maschine_jam_outputs_button_led_level_left_6_group,
	&maschine_jam_outputs_button_led_level_left_7_group,
	&maschine_jam_outputs_button_led_level_left_8_group,
	&maschine_jam_outputs_button_led_level_right_1_group,
	&maschine_jam_outputs_button_led_level_right_2_group,
	&maschine_jam_outputs_button_led_level_right_3_group,
	&maschine_jam_outputs_button_led_level_right_4_group,
	&maschine_jam_outputs_button_led_level_right_5_group,
	&maschine_jam_outputs_button_led_level_right_6_group,
	&maschine_jam_outputs_button_led_level_right_7_group,
	&maschine_jam_outputs_button_led_level_right_8_group,
	NULL
};

#define MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_outputs_pad_led_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_type_show, \
			.store = maschine_jam_outputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_channel_show, \
			.store = maschine_jam_outputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_key_show, \
			.store = maschine_jam_outputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_status_show, \
			.store = maschine_jam_outputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_OUTPUT_PAD_LED, \
		.io_index = _index, \
		.smartstrip_finger = 0, \
		.smartstrip_finger_mode = 0, \
	}; \
	static struct attribute *maschine_jam_outputs_pad_led_ ## _name ## _attributes[] = { \
		&maschine_jam_outputs_pad_led_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_outputs_pad_led_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_outputs_pad_led_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_outputs_pad_led_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_outputs_pad_led_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_outputs_pad_led_ ## _name ## _attributes, \
	}
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_1, 0);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_2, 1);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_3, 2);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_4, 3);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_5, 4);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_6, 5);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_7, 6);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(scene_8, 7);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x1, 8);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x2, 9);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x3, 10);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x4, 11);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x5, 12);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x6, 13);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x7, 14);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_1x8, 15);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x1, 16);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x2, 17);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x3, 18);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x4, 19);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x5, 20);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x6, 21);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x7, 22);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_2x8, 23);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x1, 24);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x2, 25);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x3, 26);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x4, 27);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x5, 28);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x6, 29);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x7, 30);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_3x8, 31);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x1, 32);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x2, 33);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x3, 34);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x4, 35);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x5, 36);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x6, 37);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x7, 38);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_4x8, 39);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x1, 40);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x2, 41);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x3, 42);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x4, 43);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x5, 44);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x6, 45);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x7, 46);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_5x8, 47);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x1, 48);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x2, 49);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x3, 50);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x4, 51);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x5, 52);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x6, 53);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x7, 54);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_6x8, 55);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x1, 56);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x2, 57);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x3, 58);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x4, 59);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x5, 60);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x6, 61);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x7, 62);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_7x8, 63);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x1, 64);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x2, 65);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x3, 66);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x4, 67);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x5, 68);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x6, 69);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x7, 70);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(matrix_8x8, 71);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_a, 72);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_b, 73);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_c, 74);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_d, 75);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_e, 76);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_f, 77);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_g, 78);
MJ_OUTPUTS_PAD_LED_ATTRIBUTE_GROUP(group_h, 79);

static const struct attribute_group *maschine_jam_outputs_pad_led_groups[] = {
	&maschine_jam_outputs_pad_led_scene_1_group,
	&maschine_jam_outputs_pad_led_scene_2_group,
	&maschine_jam_outputs_pad_led_scene_3_group,
	&maschine_jam_outputs_pad_led_scene_4_group,
	&maschine_jam_outputs_pad_led_scene_5_group,
	&maschine_jam_outputs_pad_led_scene_6_group,
	&maschine_jam_outputs_pad_led_scene_7_group,
	&maschine_jam_outputs_pad_led_scene_8_group,
	&maschine_jam_outputs_pad_led_matrix_1x1_group,
	&maschine_jam_outputs_pad_led_matrix_1x2_group,
	&maschine_jam_outputs_pad_led_matrix_1x3_group,
	&maschine_jam_outputs_pad_led_matrix_1x4_group,
	&maschine_jam_outputs_pad_led_matrix_1x5_group,
	&maschine_jam_outputs_pad_led_matrix_1x6_group,
	&maschine_jam_outputs_pad_led_matrix_1x7_group,
	&maschine_jam_outputs_pad_led_matrix_1x8_group,
	&maschine_jam_outputs_pad_led_matrix_2x1_group,
	&maschine_jam_outputs_pad_led_matrix_2x2_group,
	&maschine_jam_outputs_pad_led_matrix_2x3_group,
	&maschine_jam_outputs_pad_led_matrix_2x4_group,
	&maschine_jam_outputs_pad_led_matrix_2x5_group,
	&maschine_jam_outputs_pad_led_matrix_2x6_group,
	&maschine_jam_outputs_pad_led_matrix_2x7_group,
	&maschine_jam_outputs_pad_led_matrix_2x8_group,
	&maschine_jam_outputs_pad_led_matrix_3x1_group,
	&maschine_jam_outputs_pad_led_matrix_3x2_group,
	&maschine_jam_outputs_pad_led_matrix_3x3_group,
	&maschine_jam_outputs_pad_led_matrix_3x4_group,
	&maschine_jam_outputs_pad_led_matrix_3x5_group,
	&maschine_jam_outputs_pad_led_matrix_3x6_group,
	&maschine_jam_outputs_pad_led_matrix_3x7_group,
	&maschine_jam_outputs_pad_led_matrix_3x8_group,
	&maschine_jam_outputs_pad_led_matrix_4x1_group,
	&maschine_jam_outputs_pad_led_matrix_4x2_group,
	&maschine_jam_outputs_pad_led_matrix_4x3_group,
	&maschine_jam_outputs_pad_led_matrix_4x4_group,
	&maschine_jam_outputs_pad_led_matrix_4x5_group,
	&maschine_jam_outputs_pad_led_matrix_4x6_group,
	&maschine_jam_outputs_pad_led_matrix_4x7_group,
	&maschine_jam_outputs_pad_led_matrix_4x8_group,
	&maschine_jam_outputs_pad_led_matrix_5x1_group,
	&maschine_jam_outputs_pad_led_matrix_5x2_group,
	&maschine_jam_outputs_pad_led_matrix_5x3_group,
	&maschine_jam_outputs_pad_led_matrix_5x4_group,
	&maschine_jam_outputs_pad_led_matrix_5x5_group,
	&maschine_jam_outputs_pad_led_matrix_5x6_group,
	&maschine_jam_outputs_pad_led_matrix_5x7_group,
	&maschine_jam_outputs_pad_led_matrix_5x8_group,
	&maschine_jam_outputs_pad_led_matrix_6x1_group,
	&maschine_jam_outputs_pad_led_matrix_6x2_group,
	&maschine_jam_outputs_pad_led_matrix_6x3_group,
	&maschine_jam_outputs_pad_led_matrix_6x4_group,
	&maschine_jam_outputs_pad_led_matrix_6x5_group,
	&maschine_jam_outputs_pad_led_matrix_6x6_group,
	&maschine_jam_outputs_pad_led_matrix_6x7_group,
	&maschine_jam_outputs_pad_led_matrix_6x8_group,
	&maschine_jam_outputs_pad_led_matrix_7x1_group,
	&maschine_jam_outputs_pad_led_matrix_7x2_group,
	&maschine_jam_outputs_pad_led_matrix_7x3_group,
	&maschine_jam_outputs_pad_led_matrix_7x4_group,
	&maschine_jam_outputs_pad_led_matrix_7x5_group,
	&maschine_jam_outputs_pad_led_matrix_7x6_group,
	&maschine_jam_outputs_pad_led_matrix_7x7_group,
	&maschine_jam_outputs_pad_led_matrix_7x8_group,
	&maschine_jam_outputs_pad_led_matrix_8x1_group,
	&maschine_jam_outputs_pad_led_matrix_8x2_group,
	&maschine_jam_outputs_pad_led_matrix_8x3_group,
	&maschine_jam_outputs_pad_led_matrix_8x4_group,
	&maschine_jam_outputs_pad_led_matrix_8x5_group,
	&maschine_jam_outputs_pad_led_matrix_8x6_group,
	&maschine_jam_outputs_pad_led_matrix_8x7_group,
	&maschine_jam_outputs_pad_led_matrix_8x8_group,
	&maschine_jam_outputs_pad_led_group_a_group,
	&maschine_jam_outputs_pad_led_group_b_group,
	&maschine_jam_outputs_pad_led_group_c_group,
	&maschine_jam_outputs_pad_led_group_d_group,
	&maschine_jam_outputs_pad_led_group_e_group,
	&maschine_jam_outputs_pad_led_group_f_group,
	&maschine_jam_outputs_pad_led_group_g_group,
	&maschine_jam_outputs_pad_led_group_h_group,
	NULL
};

#define MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_outputs_smartstrip_led_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_type_show, \
			.store = maschine_jam_outputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_channel_show, \
			.store = maschine_jam_outputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_key_show, \
			.store = maschine_jam_outputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_outputs_status_show, \
			.store = maschine_jam_outputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_OUTPUT_SMARTSTRIP_LED, \
		.io_index = _index, \
		.smartstrip_finger = 0, \
		.smartstrip_finger_mode = 0, \
	}; \
	static struct attribute *maschine_jam_outputs_smartstrip_led_ ## _name ## _attributes[] = { \
		&maschine_jam_outputs_smartstrip_led_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_outputs_smartstrip_led_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_outputs_smartstrip_led_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_outputs_smartstrip_led_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_outputs_smartstrip_led_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_outputs_smartstrip_led_ ## _name ## _attributes, \
	}
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x01, 0);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x02, 1);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x03, 2);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x04, 3);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x05, 4);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x06, 5);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x07, 6);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x08, 7);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x09, 8);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x10, 9);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(1x11, 10);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x01, 11);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x02, 12);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x03, 13);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x04, 14);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x05, 15);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x06, 16);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x07, 17);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x08, 18);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x09, 19);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x10, 20);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(2x11, 21);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x01, 22);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x02, 23);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x03, 24);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x04, 25);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x05, 26);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x06, 27);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x07, 28);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x08, 29);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x09, 30);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x10, 31);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(3x11, 32);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x01, 33);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x02, 34);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x03, 35);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x04, 36);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x05, 37);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x06, 38);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x07, 39);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x08, 40);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x09, 41);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x10, 42);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(4x11, 43);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x01, 44);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x02, 45);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x03, 46);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x04, 47);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x05, 48);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x06, 49);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x07, 50);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x08, 51);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x09, 52);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x10, 53);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(5x11, 54);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x01, 55);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x02, 56);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x03, 57);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x04, 58);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x05, 59);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x06, 60);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x07, 61);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x08, 62);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x09, 63);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x10, 64);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(6x11, 65);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x01, 66);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x02, 67);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x03, 68);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x04, 69);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x05, 70);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x06, 71);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x07, 72);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x08, 73);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x09, 74);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x10, 75);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(7x11, 76);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x01, 77);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x02, 78);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x03, 79);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x04, 80);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x05, 81);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x06, 82);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x07, 83);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x08, 84);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x09, 85);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x10, 86);
MJ_OUTPUTS_SMARTSTRIP_LED_ATTRIBUTE_GROUP(8x11, 87);

static const struct attribute_group *maschine_jam_outputs_smartstrip_led_groups[] = {
	&maschine_jam_outputs_smartstrip_led_1x01_group,
	&maschine_jam_outputs_smartstrip_led_1x02_group,
	&maschine_jam_outputs_smartstrip_led_1x03_group,
	&maschine_jam_outputs_smartstrip_led_1x04_group,
	&maschine_jam_outputs_smartstrip_led_1x05_group,
	&maschine_jam_outputs_smartstrip_led_1x06_group,
	&maschine_jam_outputs_smartstrip_led_1x07_group,
	&maschine_jam_outputs_smartstrip_led_1x08_group,
	&maschine_jam_outputs_smartstrip_led_1x09_group,
	&maschine_jam_outputs_smartstrip_led_1x10_group,
	&maschine_jam_outputs_smartstrip_led_1x11_group,
	&maschine_jam_outputs_smartstrip_led_2x01_group,
	&maschine_jam_outputs_smartstrip_led_2x02_group,
	&maschine_jam_outputs_smartstrip_led_2x03_group,
	&maschine_jam_outputs_smartstrip_led_2x04_group,
	&maschine_jam_outputs_smartstrip_led_2x05_group,
	&maschine_jam_outputs_smartstrip_led_2x06_group,
	&maschine_jam_outputs_smartstrip_led_2x07_group,
	&maschine_jam_outputs_smartstrip_led_2x08_group,
	&maschine_jam_outputs_smartstrip_led_2x09_group,
	&maschine_jam_outputs_smartstrip_led_2x10_group,
	&maschine_jam_outputs_smartstrip_led_2x11_group,
	&maschine_jam_outputs_smartstrip_led_3x01_group,
	&maschine_jam_outputs_smartstrip_led_3x02_group,
	&maschine_jam_outputs_smartstrip_led_3x03_group,
	&maschine_jam_outputs_smartstrip_led_3x04_group,
	&maschine_jam_outputs_smartstrip_led_3x05_group,
	&maschine_jam_outputs_smartstrip_led_3x06_group,
	&maschine_jam_outputs_smartstrip_led_3x07_group,
	&maschine_jam_outputs_smartstrip_led_3x08_group,
	&maschine_jam_outputs_smartstrip_led_3x09_group,
	&maschine_jam_outputs_smartstrip_led_3x10_group,
	&maschine_jam_outputs_smartstrip_led_3x11_group,
	&maschine_jam_outputs_smartstrip_led_4x01_group,
	&maschine_jam_outputs_smartstrip_led_4x02_group,
	&maschine_jam_outputs_smartstrip_led_4x03_group,
	&maschine_jam_outputs_smartstrip_led_4x04_group,
	&maschine_jam_outputs_smartstrip_led_4x05_group,
	&maschine_jam_outputs_smartstrip_led_4x06_group,
	&maschine_jam_outputs_smartstrip_led_4x07_group,
	&maschine_jam_outputs_smartstrip_led_4x08_group,
	&maschine_jam_outputs_smartstrip_led_4x09_group,
	&maschine_jam_outputs_smartstrip_led_4x10_group,
	&maschine_jam_outputs_smartstrip_led_4x11_group,
	&maschine_jam_outputs_smartstrip_led_5x01_group,
	&maschine_jam_outputs_smartstrip_led_5x02_group,
	&maschine_jam_outputs_smartstrip_led_5x03_group,
	&maschine_jam_outputs_smartstrip_led_5x04_group,
	&maschine_jam_outputs_smartstrip_led_5x05_group,
	&maschine_jam_outputs_smartstrip_led_5x06_group,
	&maschine_jam_outputs_smartstrip_led_5x07_group,
	&maschine_jam_outputs_smartstrip_led_5x08_group,
	&maschine_jam_outputs_smartstrip_led_5x09_group,
	&maschine_jam_outputs_smartstrip_led_5x10_group,
	&maschine_jam_outputs_smartstrip_led_5x11_group,
	&maschine_jam_outputs_smartstrip_led_6x01_group,
	&maschine_jam_outputs_smartstrip_led_6x02_group,
	&maschine_jam_outputs_smartstrip_led_6x03_group,
	&maschine_jam_outputs_smartstrip_led_6x04_group,
	&maschine_jam_outputs_smartstrip_led_6x05_group,
	&maschine_jam_outputs_smartstrip_led_6x06_group,
	&maschine_jam_outputs_smartstrip_led_6x07_group,
	&maschine_jam_outputs_smartstrip_led_6x08_group,
	&maschine_jam_outputs_smartstrip_led_6x09_group,
	&maschine_jam_outputs_smartstrip_led_6x10_group,
	&maschine_jam_outputs_smartstrip_led_6x11_group,
	&maschine_jam_outputs_smartstrip_led_7x01_group,
	&maschine_jam_outputs_smartstrip_led_7x02_group,
	&maschine_jam_outputs_smartstrip_led_7x03_group,
	&maschine_jam_outputs_smartstrip_led_7x04_group,
	&maschine_jam_outputs_smartstrip_led_7x05_group,
	&maschine_jam_outputs_smartstrip_led_7x06_group,
	&maschine_jam_outputs_smartstrip_led_7x07_group,
	&maschine_jam_outputs_smartstrip_led_7x08_group,
	&maschine_jam_outputs_smartstrip_led_7x09_group,
	&maschine_jam_outputs_smartstrip_led_7x10_group,
	&maschine_jam_outputs_smartstrip_led_7x11_group,
	&maschine_jam_outputs_smartstrip_led_8x01_group,
	&maschine_jam_outputs_smartstrip_led_8x02_group,
	&maschine_jam_outputs_smartstrip_led_8x03_group,
	&maschine_jam_outputs_smartstrip_led_8x04_group,
	&maschine_jam_outputs_smartstrip_led_8x05_group,
	&maschine_jam_outputs_smartstrip_led_8x06_group,
	&maschine_jam_outputs_smartstrip_led_8x07_group,
	&maschine_jam_outputs_smartstrip_led_8x08_group,
	&maschine_jam_outputs_smartstrip_led_8x09_group,
	&maschine_jam_outputs_smartstrip_led_8x10_group,
	&maschine_jam_outputs_smartstrip_led_8x11_group,
	NULL
};

static int maschine_jam_snd_dev_free(struct snd_device *dev){
	return 0;
}
static struct snd_device_ops maschine_jam_snd_device_ops = {
	.dev_free = maschine_jam_snd_dev_free,
};
static int maschine_jam_midi_in_open(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *driver_data = substream->rmidi->private_data;

	printk(KERN_NOTICE "maschine_jam_midi_in_open() - 1\n");
	spin_lock_irq(&driver_data->midi_in_lock);
	driver_data->midi_in_substream = substream;
	spin_unlock_irq(&driver_data->midi_in_lock);
	printk(KERN_NOTICE "maschine_jam_midi_in_open() - 2\n");
	return 0;
}

static int maschine_jam_midi_in_close(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *driver_data = substream->rmidi->private_data;

	printk(KERN_NOTICE "maschine_jam_midi_in_close() - 1\n");
	spin_lock_irq(&driver_data->midi_in_lock);
	driver_data->midi_in_substream = NULL;
	spin_unlock_irq(&driver_data->midi_in_lock);
	printk(KERN_NOTICE "maschine_jam_midi_in_close() - 2\n");
	return 0;
}

static void maschine_jam_midi_in_trigger(struct snd_rawmidi_substream *substream, int up){
	unsigned long flags;
	struct maschine_jam_driver_data *driver_data = substream->rmidi->private_data;

	printk(KERN_NOTICE "maschine_jam_midi_in_trigger() - 1\n");
	spin_lock_irqsave(&driver_data->midi_in_lock, flags);
	driver_data->midi_in_up = up;
	spin_unlock_irqrestore(&driver_data->midi_in_lock, flags);
	printk(KERN_NOTICE "maschine_jam_midi_in_trigger() - 2\n");
}

// MIDI_IN operations
static struct snd_rawmidi_ops maschine_jam_midi_in_ops = {
	.open = maschine_jam_midi_in_open,
	.close = maschine_jam_midi_in_close,
	.trigger = maschine_jam_midi_in_trigger
};

static int maschine_jam_midi_out_open(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *driver_data = substream->rmidi->private_data;

	printk(KERN_NOTICE "maschine_jam_midi_out_open() - 1\n");

	spin_lock_irq(&driver_data->midi_out_lock);
	driver_data->midi_out_substream = substream;
	driver_data->midi_out_up = 0;
	spin_unlock_irq(&driver_data->midi_out_lock);

	printk(KERN_NOTICE "maschine_jam_midi_out_open() - 2\n");
	return 0;
}

static int maschine_jam_midi_out_close(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *driver_data = substream->rmidi->private_data;

	printk(KERN_NOTICE "maschine_jam_midi_out_close() - 1\n");

	spin_lock_irq(&driver_data->midi_out_lock);
	driver_data->midi_out_substream = NULL;
	driver_data->midi_out_up = 0;
	spin_unlock_irq(&driver_data->midi_out_lock);

	printk(KERN_NOTICE "maschine_jam_midi_out_close() - 2\n");
	return 0;
}

static inline uint8_t maschine_jam_convert_smartstrip_value_to_led_index(uint8_t value){
	if (value <= 6){
		return 0;
	} else if (value >= 7 && value <= 19){
		return 1;
	} else if (value >= 20 && value <= 31){
		return 2;
	} else if (value >= 32 && value <= 44){
		return 3;
	} else if (value >= 45 && value <= 57){
		return 4;
	} else if (value >= 58 && value <= 69){
		return 5;
	} else if (value >= 70 && value <= 82){
		return 6;
	} else if (value >= 83 && value <= 95){
		return 7;
	} else if (value >= 96 && value <= 107){
		return 8;
	} else if (value >= 108 && value <= 120){
		return 9;
	} else {
		return 10;
	}
}

static inline uint8_t maschine_jam_get_smartstrip_led_state(struct maschine_jam_smartstrip_display_state* smartstrip_display_state, uint8_t smartstrip_led_index){
	switch(smartstrip_display_state->mode){
		case MJ_SMARTSTRIP_DISPLAY_MODE_SINGLE:
			if (maschine_jam_convert_smartstrip_value_to_led_index(smartstrip_display_state->value) >= smartstrip_led_index){
				return smartstrip_display_state->color;
			}
			break;
		case MJ_SMARTSTRIP_DISPLAY_MODE_DUAL:
		case MJ_SMARTSTRIP_DISPLAY_MODE_PAN:
		case MJ_SMARTSTRIP_DISPLAY_MODE_DOT:
			if (maschine_jam_convert_smartstrip_value_to_led_index(smartstrip_display_state->value) == smartstrip_led_index){
				return smartstrip_display_state->color;
			}
			break;
	}
	return 0;
}

static inline void maschine_jam_refresh_hid_report_led_smartstrips(struct maschine_jam_driver_data *driver_data){
	uint8_t i;
	uint8_t smartstrip_number;
	uint8_t smartstrip_led_index;
	struct maschine_jam_smartstrip_display_state* smartstrip_display_state;
	
	for (i=0; i<MASCHINE_JAM_NUMBER_SMARTSTRIP_LEDS; i++){
		smartstrip_number = i / MASCHINE_JAM_NUMBER_LEDS_PER_SMARTSTRIP;
		smartstrip_led_index = i % MASCHINE_JAM_NUMBER_LEDS_PER_SMARTSTRIP;
		smartstrip_display_state = &driver_data->hid_report_led_smartstrips_display_states[smartstrip_number];
		driver_data->hid_report_led_smartstrips[i] = maschine_jam_get_smartstrip_led_state(smartstrip_display_state, smartstrip_led_index);
		//printk(KERN_ALERT "maschine_jam_refresh_hid_report_led_smartstrips: number: %d, index: %d, value: %d\n", smartstrip_number, smartstrip_led_index, driver_data->hid_report_led_smartstrips[i]);
	}
}

// get virtual midi data and transmit to physical maschine jam, cannot block
static void maschine_jam_midi_out_trigger(struct snd_rawmidi_substream *substream, int up){
	//static int num;
	int sequencer_status;
	unsigned long flags;
	uint8_t data;
	struct maschine_jam_driver_data *driver_data = substream->rmidi->private_data;
	struct maschine_jam_output_node* sentinal_node;
	struct maschine_jam_output_node* output_node;
	uint8_t write_value;
	struct snd_seq_event midi_event;
	unsigned int sysex_len;
	uint8_t* sysex_ptr;
	uint8_t i;

	if (up != 0) {
		while (snd_rawmidi_transmit(substream, &data, 1) == 1) {
			//printk(KERN_NOTICE "%d - out_trigger data - %d", num++, data);
			spin_lock_irqsave(&driver_data->midi_out_encoder_lock, flags);
			sequencer_status = snd_midi_event_encode_byte(driver_data->midi_out_encoder, data, &midi_event);
			spin_unlock_irqrestore(&driver_data->midi_out_encoder_lock, flags);
			//printk(KERN_NOTICE "snd_midi_event_encode: sequencer status: %d", sequencer_status);
			if (sequencer_status == 0) {
				continue;
			} else if (sequencer_status == 1) {
				if (snd_seq_ev_is_channel_type(&midi_event)){
					if (snd_seq_ev_is_note_type(&midi_event)){
						sentinal_node = &driver_data->midi_out_note_mapping[midi_event.data.note.channel][midi_event.data.note.note];
						write_value = midi_event.data.note.velocity;
					} else if (snd_seq_ev_is_control_type(&midi_event)){
						sentinal_node = &driver_data->midi_out_control_change_mapping[midi_event.data.control.channel][midi_event.data.control.param];
						write_value = midi_event.data.control.value;
					} else {
						printk(KERN_ALERT "sequencer event type is not note or control but still channel...\n");
						return;
					}
					spin_lock_irqsave(&driver_data->midi_out_mapping_lock, flags);
					output_node = sentinal_node->node_list_head;
					if (output_node == NULL){
						if (snd_seq_ev_is_note_type(&midi_event)){
							printk(KERN_NOTICE "unmapped note_event: channel:%d, note:%d, velocity:%d\n", \
								midi_event.data.note.channel,
								midi_event.data.note.note,
								midi_event.data.note.velocity
							);
						} else if (snd_seq_ev_is_control_type(&midi_event)){
							printk(KERN_NOTICE "unmapped control_event: channel:%d, param:%d, value:%d\n", \
								midi_event.data.control.channel,
								midi_event.data.control.param,
								midi_event.data.control.value
							);
						}
					} else {
						while(output_node != NULL){
							if (output_node->type == MJ_OUTPUT_BUTTON_LED_NODE){
								spin_lock(&driver_data->hid_report_led_buttons_lock);
								driver_data->hid_report_led_buttons[output_node->index] = write_value;
								spin_unlock(&driver_data->hid_report_led_buttons_lock);
								schedule_work(&driver_data->hid_report_led_buttons_work);
							}else if(output_node->type == MJ_OUTPUT_PAD_LED_NODE){
								spin_lock(&driver_data->hid_report_led_pads_lock);
								driver_data->hid_report_led_pads[output_node->index] = write_value;
								spin_unlock(&driver_data->hid_report_led_pads_lock);
								schedule_work(&driver_data->hid_report_led_pads_work);
							}else if(output_node->type == MJ_OUTPUT_SMARTSTRIP_LED_NODE){
								spin_lock(&driver_data->hid_report_led_smartstrips_lock);
								driver_data->hid_report_led_smartstrips[output_node->index] = write_value;
								spin_unlock(&driver_data->hid_report_led_smartstrips_lock);
								schedule_work(&driver_data->hid_report_led_smartstrips_work);
							}else{
								printk(KERN_NOTICE "snd_midi_event_encode: invalid node type found\n");
							}
							output_node = output_node->next;
						}
					}
					spin_unlock_irqrestore(&driver_data->midi_out_mapping_lock, flags);
				} else if (snd_seq_ev_is_variable_type(&midi_event)){
					sysex_len = midi_event.data.ext.len;
					sysex_ptr = &((uint8_t*)midi_event.data.ext.ptr)[11];
					printk(KERN_NOTICE "snd_midi_event_encode: variable event_type, length=%d\n", sysex_len);
					if (sysex_len == 20){
						printk(KERN_ALERT "maschine_jam_midi_out_trigger: data.ext.ptr[11]=%02X\n", sysex_ptr[0]);
						spin_lock(&driver_data->hid_report_led_smartstrips_lock);
						for (i=0; i<MASCHINE_JAM_NUMBER_SMARTSTRIPS; i++){
							driver_data->hid_report_led_smartstrips_display_states[i].value = sysex_ptr[i];
						}
						maschine_jam_refresh_hid_report_led_smartstrips(driver_data);
						spin_unlock(&driver_data->hid_report_led_smartstrips_lock);
						schedule_work(&driver_data->hid_report_led_smartstrips_work);
					} else if (sysex_len == 28){
						printk(KERN_ALERT "maschine_jam_midi_out_trigger: data.ext.ptr[11-12]=%02X%02X\n", sysex_ptr[0], sysex_ptr[1]);
						spin_lock(&driver_data->hid_report_led_smartstrips_lock);
						for (i=0; i<MASCHINE_JAM_NUMBER_SMARTSTRIPS; i++){
							driver_data->hid_report_led_smartstrips_display_states[i].mode = sysex_ptr[2 * i];
							driver_data->hid_report_led_smartstrips_display_states[i].color = sysex_ptr[(2*i)+1];
						}
						maschine_jam_refresh_hid_report_led_smartstrips(driver_data);
						spin_unlock(&driver_data->hid_report_led_smartstrips_lock);
						schedule_work(&driver_data->hid_report_led_smartstrips_work);
					} else {
						printk(KERN_ALERT "snd_midi_event_encode: unknwon variable event_type length:%d\n", sysex_len);
					}
				} else {
					printk(KERN_ALERT "snd_midi_event_encode: unknwon event_type:%d\n", midi_event.type);
				}
			} else if (sequencer_status < 0){
				printk(KERN_ALERT "snd_midi_event_encode: sequencer status: %d\n", sequencer_status);
			}
		}
	}else{
		printk(KERN_ALERT "midi_out_trigger: up = 0\n");
	}
	spin_lock_irqsave(&driver_data->midi_out_lock, flags);
	driver_data->midi_out_up = up;
	spin_unlock_irqrestore(&driver_data->midi_out_lock, flags);
}

// MIDI_OUT operations
static struct snd_rawmidi_ops maschine_jam_midi_out_ops = {
	.open = maschine_jam_midi_out_open,
	.close = maschine_jam_midi_out_close,
	.trigger = maschine_jam_midi_out_trigger
};
#define MASCHINE_JAM_SOUND_CARD_DEVICE_NUMBER 0
static int maschine_jam_create_sound_card(struct maschine_jam_driver_data *driver_data){
	const char shortname[] = "MASCHINEJAM";
	//const char longname[] = "Maschine Jam";
	const char longname[] = "Maschine Jam - 1";
	int error_code;
	struct snd_card *sound_card;
	struct snd_rawmidi *rawmidi_interface;

	/* Setup sound card */
	error_code = snd_card_new(&driver_data->mj_hid_device->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1, THIS_MODULE, 0, &sound_card);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to create Maschine Jam sound card.\n");
		goto return_error_code;
	}
	strncpy(sound_card->driver, shortname, sizeof(sound_card->driver));
	strncpy(sound_card->shortname, shortname, sizeof(sound_card->shortname));
	strncpy(sound_card->longname, longname, sizeof(sound_card->longname));

	/* Setup sound device */
	error_code = snd_device_new(sound_card, SNDRV_DEV_LOWLEVEL, driver_data->mj_hid_device, &maschine_jam_snd_device_ops);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to create Maschine Jam sound device.\n");
		goto failure_snd_card_free;
	}

	/* Set up rawmidi */
	error_code = snd_rawmidi_new(sound_card, sound_card->shortname, 0, 1, 1, &rawmidi_interface);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to create Maschine Jam rawmidi device.\n");
		goto failure_snd_device_free;
	}
	strncpy(rawmidi_interface->name, sound_card->longname, sizeof(rawmidi_interface->name));
	rawmidi_interface->info_flags = SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	rawmidi_interface->private_data = driver_data;
	snd_rawmidi_set_ops(rawmidi_interface, SNDRV_RAWMIDI_STREAM_INPUT, &maschine_jam_midi_in_ops);
	snd_rawmidi_set_ops(rawmidi_interface, SNDRV_RAWMIDI_STREAM_OUTPUT, &maschine_jam_midi_out_ops);

	/* Register sound card */
	error_code = snd_card_register(sound_card);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to register Maschine Jam sound card.\n");
		goto failure_snd_device_free;
	}

	driver_data->sound_card = sound_card;
	driver_data->rawmidi_interface = rawmidi_interface;
	goto return_error_code;

failure_snd_device_free:
	snd_device_free(sound_card, driver_data->mj_hid_device);
failure_snd_card_free:
	snd_card_free(sound_card);
return_error_code:
	return error_code;
}
static int maschine_jam_delete_sound_card(struct maschine_jam_driver_data *driver_data){
	int error_code = 0;
	struct snd_card *sound_card;

	if (driver_data->sound_card != NULL) {
		sound_card = driver_data->sound_card;
		driver_data->sound_card = NULL;
		snd_card_disconnect(sound_card);
		snd_device_free(sound_card, driver_data->mj_hid_device);
		snd_card_free_when_closed(sound_card);
	}

	return error_code;
}

static int maschine_jam_create_sysfs_inputs_interface(struct maschine_jam_driver_data *driver_data){
	int error_code = 0;
	struct kobject* directory_inputs = NULL;
	struct kobject* directory_inputs_knobs = NULL;
	struct kobject* directory_inputs_buttons = NULL;
	struct kobject* directory_inputs_smartstrips = NULL;
	struct kobject *device_kobject = &driver_data->mj_hid_device->dev.kobj;

	directory_inputs = kobject_create_and_add("inputs", device_kobject);
	if (directory_inputs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add inputs failed!\n");
		error_code = -1;
		goto return_error_code;
	}
	directory_inputs_knobs = kobject_create_and_add("knobs", directory_inputs);
	if (directory_inputs_knobs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add knobs failed!\n");
		error_code = -1;
		goto failure_delete_kobject_inputs;
	}
	error_code = sysfs_create_groups(directory_inputs_knobs, maschine_jam_inputs_knobs_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups knobs failed!\n");
		goto failure_delete_kobject_inputs_knobs;
	}
	directory_inputs_buttons = kobject_create_and_add("buttons", directory_inputs);
	if (directory_inputs_buttons == NULL) {
		printk(KERN_ALERT "kobject_create_and_add buttons failed!\n");
		error_code = -1;
		goto failure_remove_inputs_knobs_groups;
	}
	error_code = sysfs_create_groups(directory_inputs_buttons, maschine_jam_inputs_buttons_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups buttons failed!\n");
		goto failure_delete_kobject_inputs_buttons;
	}
	directory_inputs_smartstrips = kobject_create_and_add("smartstrips", directory_inputs);
	if (directory_inputs_smartstrips == NULL) {
		printk(KERN_ALERT "kobject_create_and_add smartstrips failed!\n");
		error_code = -1;
		goto failure_remove_inputs_buttons_groups;
	}
	error_code = sysfs_create_groups(directory_inputs_smartstrips, maschine_jam_inputs_smartstrips_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups failed!\n");
		goto failure_delete_kobject_inputs_smartstrips;
	}
	driver_data->directory_inputs = directory_inputs;
	driver_data->directory_inputs_knobs = directory_inputs_knobs;
	driver_data->directory_inputs_buttons = directory_inputs_buttons;
	driver_data->directory_inputs_smartstrips = directory_inputs_smartstrips;
	goto return_error_code;

failure_delete_kobject_inputs_smartstrips:
	kobject_del(directory_inputs_smartstrips);
failure_remove_inputs_buttons_groups:
	sysfs_remove_groups(directory_inputs_buttons, maschine_jam_inputs_buttons_groups);
failure_delete_kobject_inputs_buttons:
	kobject_del(directory_inputs_buttons);
failure_remove_inputs_knobs_groups:
	sysfs_remove_groups(directory_inputs_knobs, maschine_jam_inputs_knobs_groups);
failure_delete_kobject_inputs_knobs:
	kobject_del(directory_inputs_knobs);
failure_delete_kobject_inputs:
	kobject_del(directory_inputs);
return_error_code:
	return error_code;
}
static void maschine_jam_delete_sysfs_inputs_interface(struct maschine_jam_driver_data *driver_data){
	if (driver_data->directory_inputs_smartstrips != NULL){
		sysfs_remove_groups(driver_data->directory_inputs_smartstrips, maschine_jam_inputs_smartstrips_groups);
		kobject_del(driver_data->directory_inputs_smartstrips);
	}
	if (driver_data->directory_inputs_buttons != NULL){
		sysfs_remove_groups(driver_data->directory_inputs_buttons, maschine_jam_inputs_buttons_groups);
		kobject_del(driver_data->directory_inputs_buttons);
	}
	if (driver_data->directory_inputs_knobs != NULL){
		sysfs_remove_groups(driver_data->directory_inputs_knobs, maschine_jam_inputs_knobs_groups);
		kobject_del(driver_data->directory_inputs_knobs);
	}
	if (driver_data->directory_inputs != NULL){
		kobject_del(driver_data->directory_inputs);
	}
}
static int maschine_jam_create_sysfs_outputs_interface(struct maschine_jam_driver_data *driver_data){
	int error_code = 0;
	struct kobject* directory_outputs = NULL;
	struct kobject* directory_outputs_button_leds = NULL;
	struct kobject* directory_outputs_pad_leds = NULL;
	struct kobject* directory_outputs_smartstrip_leds = NULL;
	struct kobject *device_kobject = &driver_data->mj_hid_device->dev.kobj;

	directory_outputs = kobject_create_and_add("outputs", device_kobject);
	if (directory_outputs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add outputs failed!\n");
		error_code = -1;
		goto return_error_code;
	}
	directory_outputs_button_leds = kobject_create_and_add("button_leds", directory_outputs);
	if (directory_outputs_button_leds == NULL) {
		printk(KERN_ALERT "kobject_create_and_add buttons failed!\n");
		error_code = -1;
		goto failure_delete_kobject_outputs;
	}
	error_code = sysfs_create_groups(directory_outputs_button_leds, maschine_jam_outputs_button_led_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups buttons failed!\n");
		goto failure_delete_kobject_outputs_button_leds;
	}
	directory_outputs_pad_leds = kobject_create_and_add("pad_leds", directory_outputs);
	if (directory_outputs_pad_leds == NULL) {
		printk(KERN_ALERT "kobject_create_and_add pads failed!\n");
		error_code = -1;
		goto failure_remove_outputs_button_leds_groups;
	}
	error_code = sysfs_create_groups(directory_outputs_pad_leds, maschine_jam_outputs_pad_led_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups pads failed!\n");
		goto failure_delete_kobject_outputs_pad_leds;
	}
	directory_outputs_smartstrip_leds = kobject_create_and_add("smartstrip_leds", directory_outputs);
	if (directory_outputs_smartstrip_leds == NULL) {
		printk(KERN_ALERT "kobject_create_and_add smartstrips failed!\n");
		error_code = -1;
		goto failure_remove_outputs_pad_leds_groups;
	}
	error_code = sysfs_create_groups(directory_outputs_smartstrip_leds, maschine_jam_outputs_smartstrip_led_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups smartstrips failed!\n");
		goto failure_delete_kobject_outputs_smartstrip_leds;
	}
	driver_data->directory_outputs = directory_outputs;
	driver_data->directory_outputs_button_leds = directory_outputs_button_leds;
	driver_data->directory_outputs_pad_leds = directory_outputs_pad_leds;
	driver_data->directory_outputs_smartstrip_leds = directory_outputs_smartstrip_leds;
	goto return_error_code;

failure_delete_kobject_outputs_smartstrip_leds:
	kobject_del(directory_outputs_smartstrip_leds);
failure_remove_outputs_pad_leds_groups:
	sysfs_remove_groups(directory_outputs_pad_leds, maschine_jam_outputs_pad_led_groups);
failure_delete_kobject_outputs_pad_leds:
	kobject_del(directory_outputs_pad_leds);
failure_remove_outputs_button_leds_groups:
	sysfs_remove_groups(directory_outputs_button_leds, maschine_jam_outputs_button_led_groups);
failure_delete_kobject_outputs_button_leds:
	kobject_del(directory_outputs_button_leds);
failure_delete_kobject_outputs:
	kobject_del(directory_outputs);
return_error_code:
	return error_code;
}
static void maschine_jam_delete_sysfs_outputs_interface(struct maschine_jam_driver_data *driver_data){
	if (driver_data->directory_outputs_smartstrip_leds != NULL){
		kobject_del(driver_data->directory_outputs_smartstrip_leds);
	}
	if (driver_data->directory_outputs_pad_leds != NULL){
		kobject_del(driver_data->directory_outputs_pad_leds);
	}
	if (driver_data->directory_outputs_button_leds != NULL){
		kobject_del(driver_data->directory_outputs_button_leds);
	}
	if (driver_data->directory_outputs != NULL){
		kobject_del(driver_data->directory_outputs);
	}
}

static int maschine_jam_probe(struct hid_device *mj_hid_device, const struct hid_device_id *id){
	int error_code;
	struct usb_interface *intface = to_usb_interface(mj_hid_device->dev.parent);
	unsigned short interface_number = intface->cur_altsetting->desc.bInterfaceNumber;
	//unsigned long dd = id->driver_data;
	struct maschine_jam_driver_data *driver_data = NULL;

	printk(KERN_ALERT "Found Maschine JAM!\n");
	if (interface_number != 0){
		printk(KERN_ALERT "Unexpected interface number %d was found. Unit may be in update mode.", interface_number);
		error_code = -EBADR; /* Interface 1 is the Device Firmware Upgrade Interface */
		goto return_error_code;
	}

	driver_data = kzalloc(sizeof(struct maschine_jam_driver_data), GFP_KERNEL);
	if (driver_data == NULL) {
		printk(KERN_ALERT "kzalloc(sizeof(struct maschine_jam_driver_data), GFP_KERNEL); FAILED\n");
		error_code = -ENOMEM;
		goto return_error_code;
	}
	maschine_jam_initialize_driver_data(driver_data, mj_hid_device);
	error_code = snd_midi_event_new(128, &driver_data->midi_in_decoder);
	if (error_code == 0) {
		snd_midi_event_reset_decode(driver_data->midi_in_decoder);
		snd_midi_event_no_status(driver_data->midi_in_decoder, 0);
	} else {
		printk(KERN_ALERT "Failed to create new midi encoder!\n");
		goto failure_free_driver_data;
	}
	error_code = snd_midi_event_new(128, &driver_data->midi_out_encoder);
	if (error_code == 0) {
		snd_midi_event_reset_encode(driver_data->midi_out_encoder);
	} else {
		printk(KERN_ALERT "Failed to create new midi decoder!\n");
		goto failure_free_midi_encoder;
	}
	error_code = maschine_jam_create_sound_card(driver_data);
	if (error_code != 0){
		printk(KERN_ALERT "Failed to create sound card.\n");
		goto failure_free_midi_decoder;
	}
	error_code = maschine_jam_create_sysfs_inputs_interface(driver_data);
	if (error_code != 0){
		printk(KERN_ALERT "Failed to create sysfs inputs interface attributes.\n");
		goto failure_delete_sound_card;
	}
	error_code = maschine_jam_create_sysfs_outputs_interface(driver_data);
	if (error_code != 0){
		printk(KERN_ALERT "Failed to create sysfs outputs interface attributes.\n");
		goto failure_delete_sysfs_inputs_interface;
	}

	hid_set_drvdata(mj_hid_device, driver_data);

	// TODO validate hid fields from some report
	error_code = hid_parse(mj_hid_device);
	if (error_code != 0) {
		printk(KERN_ALERT "hid parse failed\n");
		goto failure_delete_sysfs_outputs_interface;
	}
	error_code = hid_hw_start(mj_hid_device, HID_CONNECT_DEFAULT);
	if (error_code != 0) {
		printk(KERN_ALERT "hw start failed\n");
		goto failure_delete_sysfs_outputs_interface;
	}
	error_code = hid_hw_open(mj_hid_device);
	if (error_code != 0) {
		printk(KERN_ALERT "hw open failed\n");
		goto failure_hid_hw_stop;
	}

	goto return_error_code;

failure_hid_hw_stop:
	hid_hw_stop(mj_hid_device);
failure_delete_sysfs_outputs_interface:
	maschine_jam_delete_sysfs_outputs_interface(driver_data);
failure_delete_sysfs_inputs_interface:
	maschine_jam_delete_sysfs_inputs_interface(driver_data);
failure_delete_sound_card:
	maschine_jam_delete_sound_card(driver_data);
failure_free_midi_decoder:
	snd_midi_event_free(driver_data->midi_out_encoder);
failure_free_midi_encoder:
	snd_midi_event_free(driver_data->midi_in_decoder);
failure_free_driver_data:
	kfree(driver_data);
return_error_code:
	printk(KERN_NOTICE "Maschine JAM probe() finished - %d\n", error_code);
	return error_code;
}

static void maschine_jam_remove(struct hid_device *mj_hid_device){
	struct maschine_jam_driver_data *driver_data;

	if (mj_hid_device != NULL){
		driver_data = hid_get_drvdata(mj_hid_device);

		hid_hw_stop(mj_hid_device);
		maschine_jam_delete_sound_card(driver_data);
		maschine_jam_delete_sysfs_inputs_interface(driver_data);
		maschine_jam_delete_sysfs_outputs_interface(driver_data);
		snd_midi_event_free(driver_data->midi_out_encoder);
		snd_midi_event_free(driver_data->midi_in_decoder);
		kfree(driver_data);
	}

	printk(KERN_ALERT "Maschine JAM removed!\n");
}

static const struct hid_device_id maschine_jam_devices[] = {
	{
		HID_USB_DEVICE(
			USB_VENDOR_ID_NATIVE_INSTRUMENTS,
			USB_DEVICE_ID_MASCHINE_JAM),
		.driver_data = 0
	},
	{ }
};
MODULE_DEVICE_TABLE(hid, maschine_jam_devices);

static struct hid_driver maschine_jam_driver = {
	.name = "maschine-jam",
	.id_table = maschine_jam_devices,
	.raw_event = maschine_jam_raw_event,
	.probe = maschine_jam_probe,
	.remove = maschine_jam_remove,
};
module_hid_driver(maschine_jam_driver);

MODULE_LICENSE("GPL");
