#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/hid.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>
#include "hid-ids.h"

struct maschine_jam_private_data;

struct maschine_jam {
	struct hid_device *mj_hid_device;
	struct maschine_jam_private_data *mj_private_data;
};

struct maschine_jam_private_data {
	struct maschine_jam		*mj;
	unsigned short			interface_number;
	struct snd_card			*card;
	struct snd_rawmidi		*rawmidi;
	struct snd_rawmidi_substream	*midi_in_substream;
	unsigned long			midi_in_up;
	spinlock_t				midi_in_lock;
	struct snd_rawmidi_substream	*midi_out_substream;
	unsigned long			midi_out_up;
	spinlock_t				midi_out_lock;
	unsigned long 			hid_report_led_buttons;
	spinlock_t 				hid_report_led_buttons_lock;
	struct work_struct		hid_report_led_buttons_work;
	unsigned long 			hid_report_led_pads;
	spinlock_t 				hid_report_led_pads_lock;
	struct work_struct		hid_report_led_pads_work;
	unsigned long 			hid_report_led_strips;
	spinlock_t 				hid_report_led_strips_lock;
	struct work_struct		hid_report_led_strips_work;
};

static const char shortname[] = "MASCHINEJAM";
static const char longname[] = "Maschine Jam";

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static int maschine_jam_midi_in_open(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_open()");
	mj_private_data->midi_in_substream = substream;
	return 0;
}

static int maschine_jam_midi_in_close(struct snd_rawmidi_substream *substream)
{
	unsigned long flags;
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;
	
	printk(KERN_ALERT "in_close()");
	mj_private_data->midi_in_substream = NULL;
	return 0;
}

static void maschine_jam_midi_in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_trigger()");
	mj_private_data->midi_in_up = up;
}

// MIDI_IN operations
static struct snd_rawmidi_ops maschine_jam_midi_in_ops = {
	.open = maschine_jam_midi_in_open,
	.close = maschine_jam_midi_in_close,
	.trigger = maschine_jam_midi_in_trigger
};

static int maschine_jam_midi_out_open(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_private_data *mj_private_data = substream->rmidi->private_data;

	printk(KERN_ALERT "out_open() - 1");
	
	spin_lock_irq(&mj_private_data->midi_out_lock);
	mj_private_data->midi_out_substream = substream;
	mj_private_data->midi_out_up = 0;
	spin_lock_irq(&mj_private_data->midi_out_lock);
	
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
	spin_lock_irq(&mj_private_data->midi_out_lock);
	
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
			mj_private_data->hid_report_led_buttons = data;
			spin_lock_irqrestore(&mj_private_data->hid_report_led_buttons_lock, flags);
			//hid_hw_output_report(mj_hid_device, buf, sizeof(buf));
		}
	}else{
		printk(KERN_ALERT "out_trigger: up = 0");
	}
	spin_lock_irqsave(&mj_private_data->midi_out_lock, flags);
	mj_private_data->midi_out_up = up;
	spin_lock_irqrestore(&mj_private_data->midi_out_lock, flags);
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

static void maschine_jam_write_midi_note(struct maschine_jam_private_data *mj_private_data,
	unsigned char status, unsigned char note, unsigned char velocity)
{
	int bytes_transmitted = 0;
	unsigned long flags;
	unsigned char buffer[3];

	buffer[0] = status;
	buffer[1] = note;
	buffer[2] = velocity;

	printk(KERN_ALERT "write_midi_note() - start");
	
	spin_lock_irqsave(&mj_private_data->midi_in_lock, flags);
	if (mj_private_data->midi_in_substream){
		bytes_transmitted = snd_rawmidi_receive(mj_private_data->midi_in_substream, buffer, 3);
	}
	spin_unlock_irqrestore(&mj_private_data->midi_in_lock, flags);
	printk(KERN_ALERT "snd_rawmidi_transmit() - %d", bytes_transmitted);

	return;
}

static int maschine_jam_raw_event(struct hid_device *mj_hid_device, struct hid_report *report, u8 *data, int size)
{
	struct maschine_jam *mj = hid_get_drvdata(mj_hid_device);
	
	printk(KERN_ALERT "raw_event() - size %d", size);
	if (report->id == data[0]){
		printk(KERN_ALERT "report id %d - %X%X%X%X",data[0], data[1], data[2], data[3], data[4]);
		switch (report->id) {
			case 0x01:
				if (data[2] == 0x2){
					maschine_jam_write_midi_note(mj->mj_private_data, 0x90, 0x40, 0x40);
					printk(KERN_ALERT "key-on");
				}else{
					maschine_jam_write_midi_note(mj->mj_private_data, 0x80, 0x40, 0x00);
					printk(KERN_ALERT "key-off");
				}
				break;
			case 0x02:
				break;
			default:
				printk(KERN_ALERT "report id is unknown");
				break;
		}
	}
	return 0;
}

static int maschine_jam_snd_dev_free(struct snd_device *dev)
{
	return 0;
}

static int maschine_jam_private_data_initialize(struct maschine_jam_private_data *mj_private_data)
{
	static int dev;
	struct snd_card *card;
	struct snd_rawmidi *rawmidi;
	int err;

	static struct snd_device_ops ops = {
		.dev_free = maschine_jam_snd_dev_free,
	};
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
