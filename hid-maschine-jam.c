#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/hid.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#include "hid-ids.h"

struct maschine_jam_midi_in_mapping {
	char label[64];
	uint8_t midi_note;
};

struct maschine_jam_midi_out_mapping {
	char label[64];
	uint8_t midi_note;
};

struct maschine_jam_private_data {
	struct maschine_jam		*mj;
	unsigned char			hid_report0x01[16];
	struct maschine_jam_midi_in_mapping midi_in_button_mapping[128];
	unsigned char			hid_report0x02[48];
	struct maschine_jam_midi_in_mapping midi_in_strip_byte_mapping[16];
	unsigned short			interface_number;
	struct snd_card			*card;
	struct snd_rawmidi		*rawmidi;
	struct snd_rawmidi_substream	*midi_in_substream;
	unsigned long			midi_in_up;
	spinlock_t				midi_in_lock;
	struct maschine_jam_midi_out_mapping midi_out_led_buttons_mapping[120];
	struct maschine_jam_midi_out_mapping midi_out_led_pads_mapping[120];
	struct maschine_jam_midi_out_mapping midi_out_led_strips_mapping[8];
	struct snd_rawmidi_substream	*midi_out_substream;
	unsigned long			midi_out_up;
	spinlock_t				midi_out_lock;
	struct work_struct		hid_report_write_report_work;
	unsigned char 			hid_report_led_buttons_1[37];
	unsigned char 			hid_report_led_buttons_2[16];
	spinlock_t 				hid_report_led_buttons_lock;
	struct work_struct		hid_report_led_buttons_work;
	unsigned char 			hid_report_led_pads[80];
	spinlock_t 				hid_report_led_pads_lock;
	struct work_struct		hid_report_led_pads_work;
	unsigned char 			hid_report_led_strips[88];
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

	printk(KERN_ALERT "write_midi_note() - start %X%X%X", status, note, velocity);

	spin_lock_irqsave(&mj_private_data->midi_in_lock, flags);
	if (mj_private_data->midi_in_substream){
		bytes_transmitted = snd_rawmidi_receive(mj_private_data->midi_in_substream, buffer, 3);
	}
	spin_unlock_irqrestore(&mj_private_data->midi_in_lock, flags);
	printk(KERN_ALERT "snd_rawmidi_transmit() - %d", bytes_transmitted);

	return bytes_transmitted;
}



int maschine_jam_parse_report0x01(struct maschine_jam_private_data *mj_private_data, u8 *data, int size){
	int return_value = 0;
	unsigned int button_bit, button_byte, mapping_index;

	if (mj_private_data == NULL || data == NULL || size != 16){
		printk(KERN_ALERT "maschine_jam_parse_report0x01 - ERROR01");
		return -1;
	}
	printk(KERN_ALERT "report - %X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);

	for (button_byte=0; button_byte<sizeof(mj_private_data->hid_report0x01) && button_byte<size; button_byte++){
		if (mj_private_data->hid_report0x01[button_byte] != data[button_byte]){
			for (button_bit=0; button_bit<8; button_bit++){
				if (test_bit(button_bit, (long unsigned int*)&data[button_byte]) != test_bit(button_bit, (long unsigned int*)&mj_private_data->hid_report0x01[button_byte])){
					mapping_index = button_bit + button_byte*8;
					if (test_bit(button_bit, (long unsigned int*)&data[button_byte])){
						return_value = maschine_jam_write_midi_note(mj_private_data, 0x90, mj_private_data->midi_in_button_mapping[mapping_index].midi_note, 0x40);
						printk(KERN_ALERT "key-off - mapping_index %d", mapping_index);
					}else{
						return_value = maschine_jam_write_midi_note(mj_private_data, 0x80, mj_private_data->midi_in_button_mapping[mapping_index].midi_note, 0x00);
						printk(KERN_ALERT "key-off - mapping_index %d", mapping_index);
					}
				}
			}
			mj_private_data->hid_report0x01[button_byte] = data[button_byte];
		}
	}
	return return_value;
}

void maschine_jam_parse_report0x02(struct maschine_jam_private_data *mj_private_data, u8 *data, int size){
	int strip_byte;

	for (strip_byte=0; (strip_byte*6)+1<size; strip_byte++){
		if (mj_private_data->hid_report0x02[strip_byte] != data[strip_byte]){
			maschine_jam_write_midi_note(mj_private_data, 0x90, mj_private_data->midi_in_strip_byte_mapping[strip_byte].midi_note, data[strip_byte]);
			mj_private_data->hid_report0x02[strip_byte] = data[strip_byte];
		}
		if (mj_private_data->hid_report0x02[strip_byte+1] != data[strip_byte+1]){
			maschine_jam_write_midi_note(mj_private_data, 0x90, mj_private_data->midi_in_strip_byte_mapping[strip_byte].midi_note, data[strip_byte+1]);
			mj_private_data->hid_report0x02[strip_byte+1] = data[strip_byte+1];
		}
	}
}

static int maschine_jam_raw_event(struct hid_device *mj_hid_device, struct hid_report *report, u8 *data, int size)
{
	int return_value = 0;
	struct maschine_jam *mj = hid_get_drvdata(mj_hid_device);

	printk(KERN_ALERT "maschine_jam_raw_event() - id %d", report->id);
	printk(KERN_ALERT "maschine_jam_raw_event() - size %d", size);
	if (report != NULL && data != NULL && report->id == data[0] && size >= 0){
		if (report->id == 0x01 && size == 17){
			maschine_jam_parse_report0x01(mj->mj_private_data, &data[1], 16);
		} else if (report->id == 0x02 && size == 49){
			printk(KERN_ALERT "report - %X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X",
			data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16],
			data[17], data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26], data[27], data[28], data[29], data[30], data[31], data[32], data[33],
			data[34], data[35], data[36], data[37], data[38], data[39], data[40], data[41], data[42], data[43], data[44], data[45], data[46], data[47], data[48]);
			maschine_jam_parse_report0x02(mj->mj_private_data, data, size);
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

#define NAMER "thiserz\n"

static ssize_t maschine_jam_00_sysfs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	memcpy(buf, NAMER, sizeof(NAMER));
	return sizeof(NAMER);
}
static ssize_t maschine_jam_00_sysfs_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	return 0;
}
// static DEVICE_ATTR(channelz, S_IRUGO | S_IWUSR | S_IWGRP , maschine_jam_00_sysfs_show, maschine_jam_00_sysfs_store);
// static struct device_attribute *sysfs_device_attr_channelz = {
// 		&dev_attr_channelz,
// };
static struct kobj_attribute channelz_attr = {
	.attr = {.name = "channelz_attr", .mode = 0666},
	.show = maschine_jam_00_sysfs_show,
	.store = maschine_jam_00_sysfs_store,
};
static const struct attribute *dev_attr_channelz_attrs[] = {
	&channelz_attr.attr,
	NULL
};

static int maschine_jam_private_data_initialize(struct maschine_jam_private_data *mj_private_data)
{
	static int dev;
	struct kobject* directory_inputs = NULL;
	struct kobject* directory_inputs_buttons = NULL;
	struct snd_card *card;
	struct snd_rawmidi *rawmidi;
	int err;
	unsigned int i;

	static struct snd_device_ops ops = {
		.dev_free = maschine_jam_snd_dev_free,
	};

	// Initialize midi_in button mapping
	for(i=0; i<sizeof(mj_private_data->midi_in_button_mapping)/sizeof(mj_private_data->midi_in_button_mapping[0]); i++){
		mj_private_data->midi_in_button_mapping[i].midi_note = i;
	}

	/* create sysfs variables */
	directory_inputs = kobject_create_and_add("inputs", &mj_private_data->mj->mj_hid_device->dev.kobj);
	if (directory_inputs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	directory_inputs_buttons = kobject_create_and_add("buttons", directory_inputs);
	if (directory_inputs_buttons == NULL) {
		printk(KERN_ALERT "kobject_create_and_add failed!");
	}
	err = sysfs_create_files(directory_inputs_buttons, dev_attr_channelz_attrs);
	if (err < 0) {
		printk(KERN_ALERT "device_create_file failed!");
	}

	//device_remove_file
	// void kobject_put( struct kobject  * subdir  );

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
