#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/hid.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#include "hid-ids.h"

typedef uint8_t midi_note;
typedef uint8_t midi_value;

struct maschine_jam_private_data {
	struct maschine_jam		*mj;
	uint8_t			hid_report01_knobs[1];
	uint8_t			hid_report01_buttons[15];
	midi_note midi_in_knob_mapping[2];
	midi_note midi_in_button_mapping[120];
	uint8_t			hid_report0x02[48];
	midi_note midi_in_strip_byte_mapping[16];
	unsigned short			interface_number;
	struct snd_card			*card;
	struct snd_rawmidi		*rawmidi;
	struct snd_rawmidi_substream	*midi_in_substream;
	unsigned long			midi_in_up;
	spinlock_t				midi_in_lock;
	midi_note midi_out_led_buttons_mapping[120];
	midi_note midi_out_led_pads_mapping[120];
	midi_note midi_out_led_strips_mapping[8];
	struct snd_rawmidi_substream	*midi_out_substream;
	unsigned long			midi_out_up;
	spinlock_t				midi_out_lock;
	struct work_struct		hid_report_write_report_work;
	uint8_t 			hid_report_led_buttons_1[37];
	uint8_t 			hid_report_led_buttons_2[16];
	spinlock_t 				hid_report_led_buttons_lock;
	struct work_struct		hid_report_led_buttons_work;
	uint8_t 			hid_report_led_pads[80];
	spinlock_t 				hid_report_led_pads_lock;
	struct work_struct		hid_report_led_pads_work;
	uint8_t 			hid_report_led_strips[88];
	spinlock_t 				hid_report_led_strips_lock;
	struct work_struct		hid_report_led_strips_work;
};

struct maschine_jam {
	struct hid_device *mj_hid_device;
	struct maschine_jam_private_data *mj_private_data;
};

static const char shortname[] = "MASCHINEJAM";
static const char longname[] = "Maschine Jam";

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static int maschine_jam_midi_in_open(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_open() - 1");
	spin_lock_irq(&mj_private_data->midi_in_lock);
	mj_private_data->midi_in_substream = substream;
	spin_unlock_irq(&mj_private_data->midi_in_lock);
	printk(KERN_ALERT "in_open() - 2");
	return 0;
}

static int maschine_jam_midi_in_close(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_close() - 1");
	spin_lock_irq(&mj_private_data->midi_in_lock);
	mj_private_data->midi_in_substream = NULL;
	spin_unlock_irq(&mj_private_data->midi_in_lock);
	printk(KERN_ALERT "in_close() - 2");
	return 0;
}

static void maschine_jam_midi_in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	unsigned long flags;
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_trigger() - 1");
	spin_lock_irqsave(&mj_private_data->midi_in_lock, flags);
	mj_private_data->midi_in_up = up;
	spin_unlock_irqrestore(&mj_private_data->midi_in_lock, flags);
	printk(KERN_ALERT "in_trigger() - 2");
}

// MIDI_IN operations
static struct snd_rawmidi_ops maschine_jam_midi_in_ops = {
	.open = maschine_jam_midi_in_open,
	.close = maschine_jam_midi_in_close,
	.trigger = maschine_jam_midi_in_trigger
};

static void maschine_jam_hid_write_report(struct work_struct *work)
{
	int i;
	unsigned char buffer[54];
	struct maschine_jam_private_data *mj_private_data = container_of(work, struct maschine_jam_private_data, hid_report_write_report_work);
	printk(KERN_ALERT "maschine_jam_hid_write_report() - 1");
	buffer[0] = 0x80;
	spin_lock(&mj_private_data->hid_report_led_buttons_lock);
	for(i=0;i<sizeof(mj_private_data->hid_report_led_buttons_1)/sizeof(mj_private_data->hid_report_led_buttons_1[0]); i++){
		buffer[i+1] = mj_private_data->hid_report_led_buttons_1[i];
	}
	for(i=0;i<sizeof(mj_private_data->hid_report_led_buttons_2)/sizeof(mj_private_data->hid_report_led_buttons_2[0]); i++){
		buffer[i+sizeof(mj_private_data->hid_report_led_buttons_1)/sizeof(mj_private_data->hid_report_led_buttons_1[0])] = mj_private_data->hid_report_led_buttons_2[i];
	}
	spin_unlock(&mj_private_data->hid_report_led_buttons_lock);
	hid_hw_output_report(mj_private_data->mj->mj_hid_device, (unsigned char*)&buffer, sizeof(buffer));
	printk(KERN_ALERT "maschine_jam_hid_write_report() - 2");
}

static int maschine_jam_midi_out_open(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "out_open() - 1");

	spin_lock_irq(&mj_private_data->midi_out_lock);
	mj_private_data->midi_out_substream = substream;
	mj_private_data->midi_out_up = 0;
	spin_unlock_irq(&mj_private_data->midi_out_lock);

	printk(KERN_ALERT "out_open() - 2");
	return 0;
}

static int maschine_jam_midi_out_close(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "out_close() - 1");

	spin_lock_irq(&mj_private_data->midi_out_lock);
	mj_private_data->midi_out_substream = NULL;
	mj_private_data->midi_out_up = 0;
	spin_unlock_irq(&mj_private_data->midi_out_lock);

	printk(KERN_ALERT "out_close() - 2");
	return 0;
}

// get virtual midi data and transmit to physical maschine jam, cannot block
static void maschine_jam_midi_out_trigger(struct snd_rawmidi_substream *substream, int up)
{
	unsigned long flags;
	unsigned char data;
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	if (up != 0) {
		while (snd_rawmidi_transmit(substream, &data, 1) == 1) {
			printk(KERN_ALERT "out_trigger data - %d", data);
			spin_lock_irqsave(&mj_private_data->hid_report_led_buttons_lock, flags);
			mj_private_data->hid_report_led_buttons_1[0] = data;
			spin_unlock_irqrestore(&mj_private_data->hid_report_led_buttons_lock, flags);
			schedule_work(&mj_private_data->hid_report_write_report_work);
		}
	}else{
		printk(KERN_ALERT "out_trigger: up = 0");
	}
	spin_lock_irqsave(&mj_private_data->midi_out_lock, flags);
	mj_private_data->midi_out_up = up;
	spin_unlock_irqrestore(&mj_private_data->midi_out_lock, flags);
}

// MIDI_OUT operations
static struct snd_rawmidi_ops maschine_jam_midi_out_ops = {
	.open = maschine_jam_midi_out_open,
	.close = maschine_jam_midi_out_close,
	.trigger = maschine_jam_midi_out_trigger
};

static __u8 *maschine_jam_report_fixup(struct hid_device *mj_hid_device, __u8 *rdesc,
		unsigned int *rsize)
{
	return rdesc;
}

static int maschine_jam_input_mapping(struct hid_device *mj_hid_device, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	return 0;
}

static int maschine_jam_write_midi_note(struct maschine_jam_private_data *mj_private_data,
	unsigned char status, unsigned char note, unsigned char velocity)
{
	int bytes_transmitted = 0;
	unsigned long flags;
	unsigned char buffer[3];

	buffer[0] = status;
	buffer[1] = note;
	buffer[2] = velocity;

	printk(KERN_ALERT "write_midi_note() - start %02X%02X%02X", status, note, velocity);

	spin_lock_irqsave(&mj_private_data->midi_in_lock, flags);
	if (mj_private_data->midi_in_substream){
		bytes_transmitted = snd_rawmidi_receive(mj_private_data->midi_in_substream, buffer, 3);
	}
	spin_unlock_irqrestore(&mj_private_data->midi_in_lock, flags);
	printk(KERN_ALERT "snd_rawmidi_transmit() - %d", bytes_transmitted);

	return bytes_transmitted;
}

static inline uint8_t maschine_jam_get_knob_nibble(u8 *data, uint8_t offset){
	return (data[offset / 2] >> ((offset % 2) * 4)) && 0x0F;
}
static inline void maschine_jam_set_knob_nibble(u8 *data, uint8_t offset, uint8_t value){
	data[offset / 2] = (data[offset / 2] & (0x0F << ((offset % 2) * 4))) | (value & (0xF0 >> ((offset % 2) * 4)));
}
static inline uint8_t maschine_jam_get_button_bit(u8 *data, uint8_t offset){
	return test_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static inline void maschine_jam_toggle_button_bit(u8 *data, uint8_t offset){
	change_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static int maschine_jam_parse_report01(struct maschine_jam_private_data *mj_private_data, struct hid_report *report, u8 *data, int size){
	int return_value = 0;
	struct hid_field *knobs_hid_field = report->field[0];
	struct hid_field *buttons_hid_field = report->field[1];
	unsigned int button_bit, knob_nibble, knobs_data_index, buttons_data_index;
	uint8_t old_knob_value, new_knob_value, old_button_value, new_button_value;

	printk(KERN_ALERT "report - %X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);

	knobs_data_index = knobs_hid_field->report_offset / 8;
	for (knob_nibble = 0; knob_nibble < knobs_hid_field->report_count; knob_nibble++){
		old_knob_value = maschine_jam_get_knob_nibble(mj_private_data->hid_report01_knobs, knob_nibble);
		new_knob_value = maschine_jam_get_knob_nibble(&data[knobs_data_index], knob_nibble);
		if (old_knob_value != new_knob_value){
			return_value = maschine_jam_write_midi_note(mj_private_data, 0x90, mj_private_data->midi_in_knob_mapping[knob_nibble], new_knob_value * 0x04);
			maschine_jam_set_knob_nibble(mj_private_data->hid_report01_knobs, knob_nibble, new_knob_value);
			printk(KERN_ALERT "knob_nibble: %d, new value: %d", knob_nibble, new_knob_value);
		}
	}
	buttons_data_index = buttons_hid_field->report_offset / 8;
	for (button_bit = 0; button_bit < buttons_hid_field->report_count; button_bit++){
		old_button_value = maschine_jam_get_button_bit(mj_private_data->hid_report01_buttons, button_bit);
		new_button_value = maschine_jam_get_button_bit(&data[buttons_data_index], button_bit);
		if (old_button_value != new_button_value){
			maschine_jam_toggle_button_bit(mj_private_data->hid_report01_buttons, button_bit);
			return_value = maschine_jam_write_midi_note(mj_private_data, 0x80 + (0x10 * new_button_value), mj_private_data->midi_in_button_mapping[button_bit], 0x40 * new_button_value);
			printk(KERN_ALERT "button_bit: %d, new value: %d", button_bit, new_button_value);
		}
	}
	return return_value;
}

static void maschine_jam_parse_report02(struct maschine_jam_private_data *mj_private_data, u8 *data, int size){
	int strip_byte;

	for (strip_byte=0; (strip_byte*6)+1<size; strip_byte++){
		if (mj_private_data->hid_report0x02[strip_byte] != data[strip_byte]){
			maschine_jam_write_midi_note(mj_private_data, 0x90, mj_private_data->midi_in_strip_byte_mapping[strip_byte], data[strip_byte]);
			mj_private_data->hid_report0x02[strip_byte] = data[strip_byte];
		}
		if (mj_private_data->hid_report0x02[strip_byte+1] != data[strip_byte+1]){
			maschine_jam_write_midi_note(mj_private_data, 0x90, mj_private_data->midi_in_strip_byte_mapping[strip_byte], data[strip_byte+1]);
			mj_private_data->hid_report0x02[strip_byte+1] = data[strip_byte+1];
		}
	}
}

static int maschine_jam_raw_event(struct hid_device *mj_hid_device, struct hid_report *report, u8 *data, int size)
{
	int return_value = 0;
	int i;
	struct maschine_jam *mj = hid_get_drvdata(mj_hid_device);

	printk(KERN_ALERT "maschine_jam_raw_event() - report->id %d", report->id);
	printk(KERN_ALERT "maschine_jam_raw_event() - report->maxfield %d", report->maxfield);
	for (i=0; i<report->maxfield; i++){
		printk(KERN_ALERT "maschine_jam_raw_event() - report->field[%d]->report_offset %d", i, report->field[i]->report_offset);
		printk(KERN_ALERT "maschine_jam_raw_event() - report->field[%d]->report_size %d", i, report->field[i]->report_size);
		printk(KERN_ALERT "maschine_jam_raw_event() - report->field[%d]->report_count %d", i, report->field[i]->report_count);
		printk(KERN_ALERT "maschine_jam_raw_event() - report->field[%d]->report_type %d", i, report->field[i]->report_type);
	}
	printk(KERN_ALERT "maschine_jam_raw_event() - size %d", size);
	if (report != NULL && data != NULL && report->id == data[0]){
		if (report->id == 0x01 && size == 17){
			maschine_jam_parse_report01(mj->mj_private_data, report, &data[1], 16);
		} else if (report->id == 0x02 && size == 49){
			printk(KERN_ALERT "report - %X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X",
			data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16],
			data[17], data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26], data[27], data[28], data[29], data[30], data[31], data[32], data[33],
			data[34], data[35], data[36], data[37], data[38], data[39], data[40], data[41], data[42], data[43], data[44], data[45], data[46], data[47], data[48]);
			maschine_jam_parse_report02(mj->mj_private_data, data, size);
		} else {
			printk(KERN_ALERT "maschine_jam_raw_event() - report id is unknown or bad data size");
		}
	} else {
		printk(KERN_ALERT "maschine_jam_raw_event() - bad parameters");
	}
	return return_value;
}

static int maschine_jam_snd_dev_free(struct snd_device *dev)
{
	return 0;
}

struct maschine_jam_io_attribute {
	struct kobj_attribute mapping_attribute;
	struct kobj_attribute status_attribute;
	uint8_t io_index;
};
static ssize_t maschine_jam_inputs_button_mapping_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam *mj = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, mapping_attribute);

	return scnprintf(buf, PAGE_SIZE, "%d\n", mj->mj_private_data->midi_in_button_mapping[io_attribute->io_index]);
}
static ssize_t maschine_jam_inputs_button_mapping_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	unsigned int store_value;
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam *mj = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, mapping_attribute);

	sscanf(buf, "%u", &store_value);
	mj->mj_private_data->midi_in_button_mapping[io_attribute->io_index] = store_value & 0xFF;

	return count;
}
static ssize_t maschine_jam_inputs_button_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	// struct kobject *maschine_jam_inputs_button_dir = kobj;
	// struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	// struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	// struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	// struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	// struct maschine_jam *mj = hid_get_drvdata(hdev);
	// struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, mapping_attribute);

	// STUBBED
	return scnprintf(buf, PAGE_SIZE, "\n");
}
static ssize_t maschine_jam_inputs_button_status_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	// unsigned int store_value;
	// struct kobject *maschine_jam_inputs_button_dir = kobj;
	// struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	// struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	// struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	// struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	// struct maschine_jam *mj = hid_get_drvdata(hdev);
	// struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, mapping_attribute);

	// STUBBED
	return count;
}
#define MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_inputs_button_ ## _name ## _attribute = { \
		.mapping_attribute = { \
		 	.attr = {.name = "mapping", .mode = VERIFY_OCTAL_PERMISSIONS(0644)}, \
		 	.show = maschine_jam_inputs_button_mapping_show, \
		 	.store = maschine_jam_inputs_button_mapping_store, \
		}, \
		.status_attribute = { \
		 	.attr = {.name = "status", .mode = VERIFY_OCTAL_PERMISSIONS(0644)}, \
		 	.show = maschine_jam_inputs_button_status_show, \
		 	.store = maschine_jam_inputs_button_status_store, \
		}, \
		.io_index = _index, \
	}; \
	static struct attribute *maschine_jam_inputs_button_ ## _name ## _attributes[] = { \
		&maschine_jam_inputs_button_ ## _name ## _attribute.mapping_attribute.attr, \
		&maschine_jam_inputs_button_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_inputs_button_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_inputs_button_ ## _name ## _attributes, \
	}
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(000 ,0);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(001 ,1);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(002 ,2);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(003 ,3);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(004 ,4);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(005 ,5);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(006 ,6);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(007 ,7);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(008 ,8);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(009 ,9);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(010 ,10);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(011 ,11);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(012 ,12);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(013 ,13);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(014 ,14);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(015 ,15);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(016 ,16);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(017 ,17);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(018 ,18);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(019 ,19);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(020 ,20);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(021 ,21);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(022 ,22);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(023 ,23);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(024 ,24);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(025 ,25);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(026 ,26);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(027 ,27);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(028 ,28);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(029 ,29);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(030 ,30);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(031 ,31);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(032 ,32);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(033 ,33);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(034 ,34);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(035 ,35);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(036 ,36);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(037 ,37);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(038 ,38);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(039 ,39);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(040 ,40);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(041 ,41);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(042 ,42);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(043 ,43);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(044 ,44);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(045 ,45);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(046 ,46);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(047 ,47);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(048 ,48);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(049 ,49);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(050 ,50);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(051 ,51);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(052 ,52);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(053 ,53);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(054 ,54);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(055 ,55);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(056 ,56);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(057 ,57);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(058 ,58);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(059 ,59);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(060 ,60);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(061 ,61);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(062 ,62);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(063 ,63);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(064 ,64);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(065 ,65);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(066 ,66);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(067 ,67);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(068 ,68);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(069 ,69);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(070 ,70);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(071 ,71);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(072 ,72);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(073 ,73);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(074 ,74);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(075 ,75);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(076 ,76);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(077 ,77);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(078 ,78);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(079 ,79);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(080 ,80);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(081 ,81);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(082 ,82);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(083 ,83);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(084 ,84);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(085 ,85);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(086 ,86);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(087 ,87);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(088 ,88);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(089 ,89);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(090 ,90);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(091 ,91);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(092 ,92);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(093 ,93);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(094 ,94);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(095 ,95);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(096 ,96);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(097 ,97);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(098 ,98);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(099 ,99);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(100 ,100);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(101 ,101);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(102 ,102);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(103 ,103);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(104 ,104);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(105 ,105);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(106 ,106);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(107 ,107);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(108 ,108);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(109 ,109);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(110 ,110);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(111 ,111);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(112 ,112);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(113 ,113);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(114 ,114);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(115 ,115);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(116 ,116);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(117 ,117);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(118 ,118);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(119 ,119);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(120 ,120);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(121 ,121);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(122 ,122);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(123 ,123);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(124 ,124);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(125 ,125);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(126 ,126);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(127 ,127);

static const struct attribute_group *maschine_jam_inputs_buttons_groups[] = {
	&maschine_jam_inputs_button_000_group,
	&maschine_jam_inputs_button_001_group,
	&maschine_jam_inputs_button_002_group,
	&maschine_jam_inputs_button_003_group,
	&maschine_jam_inputs_button_004_group,
	&maschine_jam_inputs_button_005_group,
	&maschine_jam_inputs_button_006_group,
	&maschine_jam_inputs_button_007_group,
	&maschine_jam_inputs_button_008_group,
	&maschine_jam_inputs_button_009_group,
	&maschine_jam_inputs_button_010_group,
	&maschine_jam_inputs_button_011_group,
	&maschine_jam_inputs_button_012_group,
	&maschine_jam_inputs_button_013_group,
	&maschine_jam_inputs_button_014_group,
	&maschine_jam_inputs_button_015_group,
	&maschine_jam_inputs_button_016_group,
	&maschine_jam_inputs_button_017_group,
	&maschine_jam_inputs_button_018_group,
	&maschine_jam_inputs_button_019_group,
	&maschine_jam_inputs_button_020_group,
	&maschine_jam_inputs_button_021_group,
	&maschine_jam_inputs_button_022_group,
	&maschine_jam_inputs_button_023_group,
	&maschine_jam_inputs_button_024_group,
	&maschine_jam_inputs_button_025_group,
	&maschine_jam_inputs_button_026_group,
	&maschine_jam_inputs_button_027_group,
	&maschine_jam_inputs_button_028_group,
	&maschine_jam_inputs_button_029_group,
	&maschine_jam_inputs_button_030_group,
	&maschine_jam_inputs_button_031_group,
	&maschine_jam_inputs_button_032_group,
	&maschine_jam_inputs_button_033_group,
	&maschine_jam_inputs_button_034_group,
	&maschine_jam_inputs_button_035_group,
	&maschine_jam_inputs_button_036_group,
	&maschine_jam_inputs_button_037_group,
	&maschine_jam_inputs_button_038_group,
	&maschine_jam_inputs_button_039_group,
	&maschine_jam_inputs_button_040_group,
	&maschine_jam_inputs_button_041_group,
	&maschine_jam_inputs_button_042_group,
	&maschine_jam_inputs_button_043_group,
	&maschine_jam_inputs_button_044_group,
	&maschine_jam_inputs_button_045_group,
	&maschine_jam_inputs_button_046_group,
	&maschine_jam_inputs_button_047_group,
	&maschine_jam_inputs_button_048_group,
	&maschine_jam_inputs_button_049_group,
	&maschine_jam_inputs_button_050_group,
	&maschine_jam_inputs_button_051_group,
	&maschine_jam_inputs_button_052_group,
	&maschine_jam_inputs_button_053_group,
	&maschine_jam_inputs_button_054_group,
	&maschine_jam_inputs_button_055_group,
	&maschine_jam_inputs_button_056_group,
	&maschine_jam_inputs_button_057_group,
	&maschine_jam_inputs_button_058_group,
	&maschine_jam_inputs_button_059_group,
	&maschine_jam_inputs_button_060_group,
	&maschine_jam_inputs_button_061_group,
	&maschine_jam_inputs_button_062_group,
	&maschine_jam_inputs_button_063_group,
	&maschine_jam_inputs_button_064_group,
	&maschine_jam_inputs_button_065_group,
	&maschine_jam_inputs_button_066_group,
	&maschine_jam_inputs_button_067_group,
	&maschine_jam_inputs_button_068_group,
	&maschine_jam_inputs_button_069_group,
	&maschine_jam_inputs_button_070_group,
	&maschine_jam_inputs_button_071_group,
	&maschine_jam_inputs_button_072_group,
	&maschine_jam_inputs_button_073_group,
	&maschine_jam_inputs_button_074_group,
	&maschine_jam_inputs_button_075_group,
	&maschine_jam_inputs_button_076_group,
	&maschine_jam_inputs_button_077_group,
	&maschine_jam_inputs_button_078_group,
	&maschine_jam_inputs_button_079_group,
	&maschine_jam_inputs_button_080_group,
	&maschine_jam_inputs_button_081_group,
	&maschine_jam_inputs_button_082_group,
	&maschine_jam_inputs_button_083_group,
	&maschine_jam_inputs_button_084_group,
	&maschine_jam_inputs_button_085_group,
	&maschine_jam_inputs_button_086_group,
	&maschine_jam_inputs_button_087_group,
	&maschine_jam_inputs_button_088_group,
	&maschine_jam_inputs_button_089_group,
	&maschine_jam_inputs_button_090_group,
	&maschine_jam_inputs_button_091_group,
	&maschine_jam_inputs_button_092_group,
	&maschine_jam_inputs_button_093_group,
	&maschine_jam_inputs_button_094_group,
	&maschine_jam_inputs_button_095_group,
	&maschine_jam_inputs_button_096_group,
	&maschine_jam_inputs_button_097_group,
	&maschine_jam_inputs_button_098_group,
	&maschine_jam_inputs_button_099_group,
	&maschine_jam_inputs_button_100_group,
	&maschine_jam_inputs_button_101_group,
	&maschine_jam_inputs_button_102_group,
	&maschine_jam_inputs_button_103_group,
	&maschine_jam_inputs_button_104_group,
	&maschine_jam_inputs_button_105_group,
	&maschine_jam_inputs_button_106_group,
	&maschine_jam_inputs_button_107_group,
	&maschine_jam_inputs_button_108_group,
	&maschine_jam_inputs_button_109_group,
	&maschine_jam_inputs_button_110_group,
	&maschine_jam_inputs_button_111_group,
	&maschine_jam_inputs_button_112_group,
	&maschine_jam_inputs_button_113_group,
	&maschine_jam_inputs_button_114_group,
	&maschine_jam_inputs_button_115_group,
	&maschine_jam_inputs_button_116_group,
	&maschine_jam_inputs_button_117_group,
	&maschine_jam_inputs_button_118_group,
	&maschine_jam_inputs_button_119_group,
	&maschine_jam_inputs_button_120_group,
	&maschine_jam_inputs_button_121_group,
	&maschine_jam_inputs_button_122_group,
	&maschine_jam_inputs_button_123_group,
	&maschine_jam_inputs_button_124_group,
	&maschine_jam_inputs_button_125_group,
	&maschine_jam_inputs_button_126_group,
	&maschine_jam_inputs_button_127_group,
	NULL
};

static int maschine_jam_private_data_initialize_sysfs(struct kobject* device_kobject){
	int err = 0;
	struct kobject* directory_inputs = NULL;
	struct kobject* directory_inputs_buttons = NULL;
	struct kobject* directory_inputs_smartstrips = NULL;
	struct kobject* directory_outputs = NULL;
	struct kobject* directory_outputs_leds = NULL;
	struct kobject* directory_outputs_leds_buttons = NULL;
	struct kobject* directory_outputs_leds_smartstrips = NULL;

	directory_inputs = kobject_create_and_add("inputs", device_kobject);
	if (directory_inputs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	directory_inputs_buttons = kobject_create_and_add("buttons", directory_inputs);
	if (directory_inputs_buttons == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	err = sysfs_create_groups(directory_inputs_buttons, maschine_jam_inputs_buttons_groups);
	if (err < 0) {
		printk(KERN_ALERT "device_create_file failed!");
	}
	directory_inputs_smartstrips = kobject_create_and_add("smartstrips", directory_inputs);
	if (directory_inputs_smartstrips == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	directory_outputs = kobject_create_and_add("outputs", device_kobject);
	if (directory_outputs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	directory_outputs_leds = kobject_create_and_add("leds", directory_outputs);
	if (directory_outputs_leds == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	directory_outputs_leds_buttons = kobject_create_and_add("buttons", directory_outputs_leds);
	if (directory_outputs_leds_buttons == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	directory_outputs_leds_smartstrips = kobject_create_and_add("smartstrips", directory_outputs_leds);
	if (directory_outputs_leds_smartstrips == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	return err;
}

static int maschine_jam_private_data_initialize(struct maschine_jam_private_data *mj_private_data)
{
	static int dev;
	struct snd_card *card;
	struct snd_rawmidi *rawmidi;
	int err;
	unsigned int i;

	static struct snd_device_ops ops = {
		.dev_free = maschine_jam_snd_dev_free,
	};

	// TODO validate hid fields from some report

	// Initialize midi_in button mapping
	for(i=0; i<sizeof(mj_private_data->midi_in_button_mapping)/sizeof(mj_private_data->midi_in_button_mapping[0]); i++){
		mj_private_data->midi_in_button_mapping[i] = i;
	}
	for(i=0; i<sizeof(mj_private_data->midi_in_knob_mapping)/sizeof(mj_private_data->midi_in_knob_mapping[0]); i++){
		mj_private_data->midi_in_knob_mapping[i] = i;
	}
	maschine_jam_private_data_initialize_sysfs(&mj_private_data->mj->mj_hid_device->dev.kobj);

	//device_remove_file

	printk(KERN_ALERT "initialize started - dev %d - interface_number %d", dev, mj_private_data->interface_number);

	if (mj_private_data->interface_number != 0){
		printk(KERN_ALERT "initialize - only set up midi device ONCE for interace 0 - %d", mj_private_data->interface_number);
		return 53; /* only set up midi device ONCE for interace 1 */
	}

	if (dev >= SNDRV_CARDS){
		printk(KERN_ALERT "initialize - -ENODEV - dev = %d", dev);
		return -ENODEV;
	}

	if (!enable[dev]){
		printk(KERN_ALERT "initialize - -ENOENT");
		dev++;
		return -ENOENT;
	}

	/* Setup sound card */
	err = snd_card_new(&mj_private_data->mj->mj_hid_device->dev, index[dev], id[dev],
			   THIS_MODULE, 0, &card);
	if (err < 0) {
		printk(KERN_ALERT "failed to create maschine jam sound card");
		err = -ENOMEM;
		goto fail;
	}
	mj_private_data->card = card;

	/* Setup sound device */
	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, mj_private_data, &ops);
	if (err < 0) {
		printk(KERN_ALERT "ailed to create maschine jam sound device");
		goto fail;
	}

	strncpy(card->driver, shortname, sizeof(card->driver));
	strncpy(card->shortname, shortname, sizeof(card->shortname));
	strncpy(card->longname, longname, sizeof(card->longname));

	/* Set up rawmidi */
	err = snd_rawmidi_new(card, card->shortname, 0, 1, 1, &rawmidi);
	if (err < 0) {
		printk(KERN_ALERT "failed to create maschine jam rawmidi device");
		goto fail;
	}
	mj_private_data->rawmidi = rawmidi;
	strncpy(rawmidi->name, card->longname, sizeof(rawmidi->name));
	rawmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	rawmidi->private_data = mj_private_data;

	snd_rawmidi_set_ops(rawmidi, SNDRV_RAWMIDI_STREAM_INPUT, &maschine_jam_midi_in_ops);
	snd_rawmidi_set_ops(rawmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &maschine_jam_midi_out_ops);

	spin_lock_init(&mj_private_data->midi_in_lock);
	spin_lock_init(&mj_private_data->midi_out_lock);

	/* register it */
	err = snd_card_register(card);
	if (err < 0) {
		printk(KERN_ALERT "failed to register maschine jam sound card");
		goto fail;
	}

	INIT_WORK(&mj_private_data->hid_report_write_report_work, maschine_jam_hid_write_report);

	memset(mj_private_data->hid_report_led_buttons_1, 0, sizeof(mj_private_data->hid_report_led_buttons_1));
	memset(mj_private_data->hid_report_led_buttons_2, 0, sizeof(mj_private_data->hid_report_led_buttons_2));
	spin_lock_init(&mj_private_data->hid_report_led_buttons_lock);
	INIT_WORK(&mj_private_data->hid_report_led_buttons_work, maschine_jam_hid_write_report);

	memset(mj_private_data->hid_report_led_pads, 0, sizeof(mj_private_data->hid_report_led_pads));
	spin_lock_init(&mj_private_data->hid_report_led_pads_lock);
	INIT_WORK(&mj_private_data->hid_report_led_pads_work, maschine_jam_hid_write_report);

	memset(mj_private_data->hid_report_led_strips, 0, sizeof(mj_private_data->hid_report_led_strips));
	spin_lock_init(&mj_private_data->hid_report_led_strips_lock);
	INIT_WORK(&mj_private_data->hid_report_led_strips_work, maschine_jam_hid_write_report);

	printk(KERN_ALERT "initialize finished");
	return 0;

fail:
	if (mj_private_data->card) {
		snd_card_free(mj_private_data->card);
		mj_private_data->card = NULL;
	}
	return err;
}

static int maschine_jam_private_data_deinitialize(struct maschine_jam_private_data *mj_private_data)
{
	if (mj_private_data->card) {
		snd_card_disconnect(mj_private_data->card);
		snd_card_free_when_closed(mj_private_data->card);
	}

	return 0;
}

static int maschine_jam_probe(struct hid_device *mj_hid_device, const struct hid_device_id *id)
{
	int ret;
	struct usb_interface *intface = to_usb_interface(mj_hid_device->dev.parent);
	unsigned short interface_number = intface->cur_altsetting->desc.bInterfaceNumber;
	//unsigned long dd = id->driver_data;
	struct maschine_jam *mj;
	struct maschine_jam_private_data *mj_private_data = NULL;

	printk(KERN_ALERT "Found Maschine JAM!");

	mj = kzalloc(sizeof(*mj), GFP_KERNEL);
	if (mj == NULL) {
		printk(KERN_ALERT "can't alloc descriptor");
		return -ENOMEM;
	}

	mj->mj_hid_device = mj_hid_device;
	printk(KERN_ALERT "probe() - 1");

	mj_private_data = kzalloc(sizeof(*mj_private_data), GFP_KERNEL);
	if (mj_private_data == NULL) {
		printk(KERN_ALERT "can't alloc descriptor");
		ret = -ENOMEM;
		goto err_free_mj_dev;
	}

	mj_private_data->mj = mj;
	mj->mj_private_data = mj_private_data;
	mj_private_data->interface_number = interface_number;

	hid_set_drvdata(mj_hid_device, mj);
	printk(KERN_ALERT "probe() - 2");

	ret = hid_parse(mj_hid_device);
	if (ret) {
		printk(KERN_ALERT "hid parse failed");
		goto err_free;
	}

	ret = hid_hw_start(mj_hid_device, HID_CONNECT_DEFAULT);
	if (ret) {
		printk(KERN_ALERT "hw start failed");
		goto err_free;
	}
	printk(KERN_ALERT "probe() - 3");

	ret = hid_hw_open(mj_hid_device);
	if (ret) {
		printk(KERN_ALERT "hw open failed");
		goto err_free;
	}
	printk(KERN_ALERT "probe() - 4");

	ret = maschine_jam_private_data_initialize(mj_private_data);
	if (ret < 0)
		goto err_stop;

	printk(KERN_ALERT "Maschine JAM probe() finished - %d", ret);

	return 0;
err_stop:
	hid_hw_stop(mj_hid_device);
err_free:
	kfree(mj_private_data);
err_free_mj_dev:
	kfree(mj);

	return ret;
}

static void maschine_jam_remove(struct hid_device *mj_hid_device)
{
	struct maschine_jam *mj = hid_get_drvdata(mj_hid_device);
	struct maschine_jam_private_data *mj_private_data = mj->mj_private_data;

	maschine_jam_private_data_deinitialize(mj_private_data);
	hid_hw_stop(mj_hid_device);
	kfree(mj_private_data);
	kfree(mj);

	printk(KERN_ALERT "Maschine JAM removed!");
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
	.report_fixup = maschine_jam_report_fixup,
	.input_mapping = maschine_jam_input_mapping,
	.raw_event = maschine_jam_raw_event,
	.probe = maschine_jam_probe,
	.remove = maschine_jam_remove,
};
module_hid_driver(maschine_jam_driver);

MODULE_LICENSE("GPL");
