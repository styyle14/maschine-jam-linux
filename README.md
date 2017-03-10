# maschine-jam-linux
Maschine Jam HID Driver for Linux

Work in progress. Leave me a bug report or message if you would like to help out!

```
$ dmesg | tail -n 8
[ 4365.533889] usb 3-2: new full-speed USB device number 7 using xhci_hcd
[ 4365.688430] usb 3-2: New USB device found, idVendor=17cc, idProduct=1500
[ 4365.688440] usb 3-2: New USB device strings: Mfr=1, Product=2, SerialNumber=3
[ 4365.688445] usb 3-2: Product: Maschine Jam
[ 4365.688450] usb 3-2: Manufacturer: Native Instruments
[ 4365.688454] usb 3-2: SerialNumber: 120AEAEF
[ 4365.755140] hid-generic 0003:17CC:1500.000B: hiddev0,hidraw4: USB HID v1.10 Device [Native Instruments Maschine Jam] on usb-0000:04:00.0-2/input0
[ 4410.618380] usb 3-2: USB disconnect, device number 7
```
```
$ sudo lsusb -v -d 17cc:1500

Bus 003 Device 006: ID 17cc:1500 Native Instruments 
Device Descriptor:
  bLength                18
  bDescriptorType         1
  bcdUSB               2.00
  bDeviceClass            0 (Defined at Interface level)
  bDeviceSubClass         0 
  bDeviceProtocol         0 
  bMaxPacketSize0        64
  idVendor           0x17cc Native Instruments
  idProduct          0x1500 
  bcdDevice            0.39
  iManufacturer           1 Native Instruments
  iProduct                2 Maschine Jam
  iSerial                 3 120AEAEF
  bNumConfigurations      1
  Configuration Descriptor:
    bLength                 9
    bDescriptorType         2
    wTotalLength           59
    bNumInterfaces          2
    bConfigurationValue     1
    iConfiguration          0 
    bmAttributes         0x80
      (Bus Powered)
    MaxPower              480mA
    Interface Descriptor:
      bLength                 9
      bDescriptorType         4
      bInterfaceNumber        0
      bAlternateSetting       0
      bNumEndpoints           2
      bInterfaceClass         3 Human Interface Device
      bInterfaceSubClass      0 No Subclass
      bInterfaceProtocol      0 None
      iInterface              4 Maschine Jam HID
        HID Device Descriptor:
          bLength                 9
          bDescriptorType        33
          bcdHID               1.10
          bCountryCode            0 Not supported
          bNumDescriptors         1
          bDescriptorType        34 Report
          wDescriptorLength     550
         Report Descriptors: 
           ** UNAVAILABLE **
      Endpoint Descriptor:
        bLength                 7
        bDescriptorType         5
        bEndpointAddress     0x81  EP 1 IN
        bmAttributes            3
          Transfer Type            Interrupt
          Synch Type               None
          Usage Type               Data
        wMaxPacketSize     0x0040  1x 64 bytes
        bInterval               1
      Endpoint Descriptor:
        bLength                 7
        bDescriptorType         5
        bEndpointAddress     0x01  EP 1 OUT
        bmAttributes            3
          Transfer Type            Interrupt
          Synch Type               None
          Usage Type               Data
        wMaxPacketSize     0x0040  1x 64 bytes
        bInterval               1
    Interface Descriptor:
      bLength                 9
      bDescriptorType         4
      bInterfaceNumber        1
      bAlternateSetting       0
      bNumEndpoints           0
      bInterfaceClass       254 Application Specific Interface
      bInterfaceSubClass      1 Device Firmware Update
      bInterfaceProtocol      1 
      iInterface              5 Maschine Jam DFU
      Device Firmware Upgrade Interface Descriptor:
        bLength                             9
        bDescriptorType                    33
        bmAttributes                        7
          Will Not Detach
          Manifestation Tolerant
          Upload Supported
          Download Supported
        wDetachTimeout                    250 milliseconds
        wTransferSize                      64 bytes
        bcdDFUVersion                   1.10
Device Status:     0x0000
  (Bus Powered)
```
```
sudo usbhid-dump -m 17cc:1500 | grep -v : | xxd -r -p | hidrd-convert -o spec
[sudo] password for jim: 
Usage Page (FF01h),                     ; FF01h, vendor-defined
Usage (00h),
Collection (Application),
    Usage (01h),
    Collection (Logical),
        Report ID (1),
        Usage (03h),
        Logical Minimum (0),
        Logical Maximum (15),
        Report Size (4),
        Report Count (2),
        Input (Variable),
        Usage (02h),
        Logical Minimum (0),
        Logical Maximum (1),
        Report Size (1),
        Report Count (120),
        Input (Variable),
    End Collection,
    Usage (01h),
    Collection (Logical),
        Report ID (2),
        Usage (44h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (24),
        Input (Variable),
    End Collection,
    Usage (40h),
    Collection (Logical),
        Report ID (64),
        Usage (41h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (12),
        Input (Variable),
    End Collection,
    Usage (80h),
    Collection (Logical),
        Report ID (128),
        Usage (81h),
        Logical Minimum (0),
        Logical Maximum (127),
        Report Size (8),
        Report Count (37),
        Output (Variable),
        Usage (81h),
        Logical Minimum (0),
        Logical Maximum (127),
        Report Size (8),
        Report Count (16),
        Output (Variable),
    End Collection,
    Usage (80h),
    Collection (Logical),
        Report ID (129),
        Usage (83h),
        Logical Minimum (0),
        Logical Maximum (127),
        Report Size (8),
        Report Count (80),
        Output (Variable),
    End Collection,
    Usage (80h),
    Collection (Logical),
        Report ID (130),
        Usage (83h),
        Logical Minimum (0),
        Logical Maximum (127),
        Report Size (8),
        Report Count (88),
        Output (Variable),
    End Collection,
    Usage (D0h),
    Collection (Logical),
        Report ID (208),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (1),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (27),
        Feature (Variable),
    End Collection,
    Usage (D8h),
    Collection (Logical),
        Report ID (216),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (4),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (12),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
    End Collection,
    Usage (D8h),
    Collection (Logical),
        Report ID (217),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (32),
        Feature (Constant, Variable),
    End Collection,
    Usage (D8h),
    Collection (Logical),
        Report ID (218),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (24),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (4),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
    End Collection,
    Usage (D8h),
    Collection (Logical),
        Report ID (219),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (24),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (4),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
    End Collection,
    Usage (D8h),
    Collection (Logical),
        Report ID (220),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (24),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (4),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
    End Collection,
    Usage (D8h),
    Collection (Logical),
        Report ID (221),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (24),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (4),
        Feature (Variable),
        Usage (D1h),
        Logical Minimum (0),
        Logical Maximum (65535),
        Report Size (16),
        Report Count (2),
        Feature (Variable),
    End Collection,
    Usage (F0h),
    Collection (Logical),
        Report ID (241),
        Usage (81h),
        Logical Minimum (0),
        Logical Maximum (127),
        Report Size (8),
        Report Count (204),
        Feature (Variable),
    End Collection,
    Usage (F0h),
    Collection (Logical),
        Report ID (242),
        Usage (81h),
        Logical Minimum (0),
        Logical Maximum (127),
        Report Size (8),
        Report Count (204),
        Feature (Variable),
    End Collection,
    Usage (F0h),
    Collection (Logical),
        Report ID (244),
        Usage (F4h),
        Logical Minimum (0),
        Logical Maximum (255),
        Report Size (8),
        Report Count (3),
        Output (Variable),
    End Collection,
End Collection
```
