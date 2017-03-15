#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/hid.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>
#include "hid-ids.h"

struct maschine_jam_snd;

struct maschine_jam_device {
	unsigned long quirks;
	
	struct hid_device *hdev;
	struct maschine_jam_snd	*mjs; /* maschine-jam device context */
};

struct maschine_jam_snd {
	struct maschine_jam_device		*mjd;
	unsigned short			ifnum;
	struct hid_report		*report;
	struct input_dev		*in_dev;
	unsigned short			midi_mode;
	unsigned short			midi_channel;
	unsigned short			fn_state;
	unsigned short			last_key[24];
	spinlock_t			rawmidi_in_lock;
	struct snd_card			*card;
	struct snd_rawmidi		*rawmidi;
	struct snd_rawmidi_substream	*midi_in_substream;
	struct snd_rawmidi_substream	*midi_out_substream;
	unsigned long			in_triggered;
	unsigned long			out_active;
};

static const char shortname[] = "MASCHINEJAM";
static const char longname[] = "Maschine Jam";

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static int maschine_jam_midi_in_open(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_snd *mjs = substream->rmidi->private_data;

	printk(KERN_ALERT "in_open()");
	mjs->midi_in_substream = substream;
	return 0;
}

static int maschine_jam_midi_in_close(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_snd *mjs = substream->rmidi->private_data;
	
	printk(KERN_ALERT "in_close()");
	mjs->midi_in_substream = NULL;
	return 0;
}

static void maschine_jam_midi_in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct maschine_jam_snd *mjs = substream->rmidi->private_data;

	printk(KERN_ALERT "in_trigger()");
	mjs->in_triggered = up;
}

static struct snd_rawmidi_ops maschine_jam_midi_in_ops = {
	.open = maschine_jam_midi_in_open,
	.close = maschine_jam_midi_in_close,
	.trigger = maschine_jam_midi_in_trigger
};

static int maschine_jam_midi_out_open(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_snd *mjs = substream->rmidi->private_data;

	printk(KERN_ALERT "out_open()");
	mjs->midi_out_substream = substream;
	return 0;
}

static int maschine_jam_midi_out_close(struct snd_rawmidi_substream *substream)
{
	struct maschine_jam_snd *mjs = substream->rmidi->private_data;
	
	printk(KERN_ALERT "out_close()");
	mjs->midi_out_substream = NULL;
	return 0;
}

// get virtual midi data and transmit to physical maschine jam, cannot block
static void maschine_jam_midi_out_trigger(struct snd_rawmidi_substream *substream, int up)
{
	unsigned char data;
	if (up != 0) {
		while (snd_rawmidi_transmit(substream, &data, 1) == 1) {
			printk(KERN_ALERT "out_trigger data - %d", data);
		}
	}
}

// maschine jam output, rawmidi input
static struct snd_rawmidi_ops maschine_jam_midi_out_ops = {
	.open = maschine_jam_midi_out_open,
	.close = maschine_jam_midi_out_close,
	.trigger = maschine_jam_midi_out_trigger
};

static __u8 *maschine_jam_report_fixup(struct hid_device *hdev, __u8 *rdesc,
		unsigned int *rsize)
{
	return rdesc;
}

static int maschine_jam_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	return 0;
}

static void maschine_jam_write_midi_note(struct maschine_jam_snd *mj_snd,
	unsigned char status, unsigned char note, unsigned char velocity)
{
	int bytes_transmitted = 0;
	unsigned long flags;
	unsigned char buffer[3];

	buffer[0] = status;
	buffer[1] = note;
	buffer[2] = velocity;

	printk(KERN_ALERT "write_midi_note() - start");
	
	spin_lock_irqsave(&mj_snd->rawmidi_in_lock, flags);
	if (mj_snd->midi_in_substream){
		bytes_transmitted = snd_rawmidi_receive(mj_snd->midi_in_substream, buffer, 3);
	}
	spin_unlock_irqrestore(&mj_snd->rawmidi_in_lock, flags);
	printk(KERN_ALERT "snd_rawmidi_transmit() - %d", bytes_transmitted);

	return;
}

static int maschine_jam_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	struct maschine_jam_device *mjd = hid_get_drvdata(hdev);
	
	printk(KERN_ALERT "raw_event() - size %d", size);
	if (report->id == data[0]){
		printk(KERN_ALERT "report id %d - %X%X%X%X",data[0], data[1], data[2], data[3], data[4]);
		switch (report->id) {
			case 0x01:
				if (data[2] == 0x2){
					maschine_jam_write_midi_note(mjd->mjs, 0x90, 0x40, 0x40);
					printk(KERN_ALERT "key-on");
				}else{
					maschine_jam_write_midi_note(mjd->mjs, 0x80, 0x40, 0x00);
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

static int maschine_jam_snd_free(struct snd_device *dev)
{
	return 0;
}

static int maschine_jam_snd_initialize(struct maschine_jam_snd *mjs)
{
	static int dev;
	struct snd_card *card;
	struct snd_rawmidi *rawmidi;
	int err;

	static struct snd_device_ops ops = {
		.dev_free = maschine_jam_snd_free,
	};
	printk(KERN_ALERT "initialize started - dev %d - ifnum %d", dev, mjs->ifnum);

	if (mjs->ifnum != 0){
		printk(KERN_ALERT "initialize - only set up midi device ONCE for interace 0 - %d", mjs->ifnum);
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
	err = snd_card_new(&mjs->mjd->hdev->dev, index[dev], id[dev],
			   THIS_MODULE, 0, &card);
	if (err < 0) {
		printk(KERN_ALERT "failed to create maschine jam sound card");
		err = -ENOMEM;
		goto fail;
	}
	mjs->card = card;

	/* Setup sound device */
	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, mjs, &ops);
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
	mjs->rawmidi = rawmidi;
	strncpy(rawmidi->name, card->longname, sizeof(rawmidi->name));
	rawmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	rawmidi->private_data = mjs;
	
	snd_rawmidi_set_ops(rawmidi, SNDRV_RAWMIDI_STREAM_INPUT, &maschine_jam_midi_in_ops);
	snd_rawmidi_set_ops(rawmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &maschine_jam_midi_out_ops);

	spin_lock_init(&mjs->rawmidi_in_lock);

	/* register it */
	err = snd_card_register(card);
	if (err < 0) {
		printk(KERN_ALERT "failed to register maschine jam sound card");
		goto fail;
	}
	printk(KERN_ALERT "initialize finished");
	return 0;

fail:
	if (mjs->card) {
		snd_card_free(mjs->card);
		mjs->card = NULL;
	}
	return err;
}

static int maschine_jam_snd_terminate(struct maschine_jam_snd *mjs)
{
	if (mjs->card) {
		snd_card_disconnect(mjs->card);
		snd_card_free_when_closed(mjs->card);
	}

	return 0;
}

static int maschine_jam_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	unsigned short ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	//unsigned long dd = id->driver_data;
	struct maschine_jam_device *mj_dev;
	struct maschine_jam_snd *mj_snd = NULL;
	
	printk(KERN_ALERT "Found Maschine JAM!");

	mj_dev = kzalloc(sizeof(*mj_dev), GFP_KERNEL);
	if (mj_dev == NULL) {
		printk(KERN_ALERT "can't alloc descriptor");
		return -ENOMEM;
	}

	mj_dev->hdev = hdev;
	printk(KERN_ALERT "probe() - 1");

	mj_snd = kzalloc(sizeof(*mj_snd), GFP_KERNEL);
	if (mj_snd == NULL) {
		printk(KERN_ALERT "can't alloc descriptor");
		ret = -ENOMEM;
		goto err_free_mj_dev;
	}

	mj_snd->mjd = mj_dev;
	mj_dev->mjs = mj_snd;
	mj_snd->ifnum = ifnum;

	hid_set_drvdata(hdev, mj_dev);
	printk(KERN_ALERT "probe() - 2");

	ret = hid_parse(hdev);
	if (ret) {
		printk(KERN_ALERT "hid parse failed");
		goto err_free;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		printk(KERN_ALERT "hw start failed");
		goto err_free;
	}
	printk(KERN_ALERT "probe() - 3");

	ret = hid_hw_open(hdev);
	if (ret) {
		printk(KERN_ALERT "hw open failed");
		goto err_free;
	}
	printk(KERN_ALERT "probe() - 4");

	ret = maschine_jam_snd_initialize(mj_snd);
	if (ret < 0)
		goto err_stop;
	
	printk(KERN_ALERT "Maschine JAM probe() finished - %d", ret);
	
	return 0;
err_stop:
	hid_hw_stop(hdev);
err_free:
	kfree(mj_snd);
err_free_mj_dev:
	kfree(mj_dev);

	return ret;
}

static void maschine_jam_remove(struct hid_device *hdev)
{
	struct maschine_jam_device *mjd = hid_get_drvdata(hdev);
	struct maschine_jam_snd *mjs;
	mjs = mjd->mjs;
	
	maschine_jam_snd_terminate(mjs);
	hid_hw_stop(hdev);
	kfree(mjs);
	kfree(mjd);
	
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
