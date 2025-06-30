/*
 * hid_injector_v2.c (Legacy Gadget Driver)
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb/gadget.h>
#include <linux/hid.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h> // For msecs_to_jiffies
#include <linux/string.h>

#define DRIVER_NAME "hid_injector_gadget"
#define DEVICE_NAME "hid_injector"
#define CLASS_NAME  "hid_injector_class"
#define MOD_LEFT_SHIFT 0x02  /* Add this line */


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lucas");
MODULE_DESCRIPTION("A self-contained USB HID keystroke injector (legacy gadget API).");
MODULE_VERSION("7.3-stable");

/* Main device structure */
struct hid_injector_dev {
    struct usb_gadget *gadget;
    struct usb_request *req0;       /* For control endpoint requests */
    struct usb_ep *in_ep;           /* Interrupt IN endpoint */
    int major;                      /* Character device major number */
    struct class *dev_class;        /* Device class */
    bool interface_active;
    struct delayed_work set_config_work; /* Use delayed work for UDC race */
    char *user_space_msg;           /* Buffer for message from user-space */
};

static struct hid_injector_dev *g_hid_dev;

/* Forward Declarations - just a C thing lol */
static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char __user *, size_t, loff_t *);
static ssize_t dev_read(struct file *, char __user *, size_t, loff_t *);
static void hid_set_config_work_handler(struct work_struct *w);
static u8 char_to_hid_keycode(const char c, u8 *modifier);
static void hid_injector_complete(struct usb_ep *ep, struct usb_request *req);
static int hid_injector_send_report(struct hid_injector_dev *dev, u8 *report);

/* --- USB Descriptors --- */
/**
 * Genai transparency notice:
 * AI Generated, due to the complexity of managing proper device descriptors to USB.
 * If even one of this is improper, the driver will FAIL.
 * So I needed some help :(
 */
static struct usb_device_descriptor device_desc = {
    .bLength            = sizeof(device_desc),
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = cpu_to_le16(0x0200),
    .bDeviceClass       = 0, /* Specified in interface descriptor */
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = 64, /* EP0 size */
    .idVendor           = cpu_to_le16(0x1d6b), /* Linux Foundation */
    .idProduct          = cpu_to_le16(0x0137), /* Keyboard Gadget */
    .bcdDevice          = cpu_to_le16(0x0100),
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

static const u8 hid_report_desc[] = {
    0x05, 0x01, /* USAGE_PAGE (Generic Desktop) */
    0x09, 0x06, /* USAGE (Keyboard) */
    0xa1, 0x01, /* COLLECTION (Application) */
    0x05, 0x07, /*   USAGE_PAGE (Keyboard) */
    0x19, 0xe0, /*   USAGE_MINIMUM (Keyboard LeftControl) */
    0x29, 0xe7, /*   USAGE_MAXIMUM (Keyboard Right GUI) */
    0x15, 0x00, /*   LOGICAL_MINIMUM (0) */
    0x25, 0x01, /*   LOGICAL_MAXIMUM (1) */
    0x75, 0x01, /*   REPORT_SIZE (1) */
    0x95, 0x08, /*   REPORT_COUNT (8) */
    0x81, 0x02, /*   INPUT (Data,Var,Abs) - Modifier keys */
    0x95, 0x01, /*   REPORT_COUNT (1) */
    0x75, 0x08, /*   REPORT_SIZE (8) */
    0x81, 0x03, /*   INPUT (Cnst,Var,Abs) - Reserved byte */
    0x95, 0x06, /*   REPORT_COUNT (6) */
    0x75, 0x08, /*   REPORT_SIZE (8) */
    0x15, 0x00, /*   LOGICAL_MINIMUM (0) */
    0x25, 0x65, /*   LOGICAL_MAXIMUM (101) */
    0x05, 0x07, /*   USAGE_PAGE (Keyboard) */
    0x19, 0x00, /*   USAGE_MINIMUM (Reserved (no event indicated)) */
    0x29, 0x65, /*   USAGE_MAXIMUM (Keyboard Application) */
    0x81, 0x00, /*   INPUT (Data,Ary,Abs) - Keycodes */
    0xc0        /* END_COLLECTION */
};

/* Raw descriptor array to avoid compiler padding issues */
static const u8 config_desc_raw[] = {
    /* Configuration Descriptor */
    0x09, USB_DT_CONFIG, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    /* Interface Descriptor */
    0x09, USB_DT_INTERFACE, 0x00, 0x00, 0x01, USB_CLASS_HID, 0x01, 0x01, 0x00,
    /* HID Descriptor */
    0x09, HID_DT_HID, 0x11, 0x01, 0x00, 0x01, 0x22, sizeof(hid_report_desc), 0x00,
    /* Endpoint Descriptor (Interrupt IN) */
    0x07, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_XFER_INT, 0x08, 0x00, 0x01,
};

static const struct usb_string strings[] = {
    { .id = 1, .s = "Lucas" },
    { .id = 2, .s = "HID Injector Gadget" },
    { .id = 3, .s = "0123456789" },
    {} /* Terminating entry */
};

static const struct usb_gadget_strings stringtab = {
    .language = 0x0409, /* en-US */
    .strings = (struct usb_string *)strings,
};

/* character device implementation */
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .write = dev_write,
    .read = dev_read,
};

static int dev_open(struct inode *inode, struct file *file)
{
    file->private_data = g_hid_dev;
    return 0;
}

static int dev_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static ssize_t dev_write(struct file *file, const char __user *buffer, size_t len, loff_t *offset)
{
    struct hid_injector_dev *dev = file->private_data;
    char *kbd_buf;
    u8 report[8] = {0};
    int i;

    if (!dev) {
        return -ENODEV;
    }

    kbd_buf = memdup_user(buffer, len);
    if (IS_ERR(kbd_buf)) {
        return PTR_ERR(kbd_buf);
    }

    pr_info("%s: Received string to type: %.*s\n", DRIVER_NAME, (int)len, kbd_buf);

    for (i = 0; i < len; i++) {
        u8 modifier = 0;
        u8 keycode = char_to_hid_keycode(kbd_buf[i], &modifier);

        if (keycode == 0) {
            pr_warn("%s: Skipping unsupported character '%c'\n", DRIVER_NAME, kbd_buf[i]);
            continue;
        }

        /* 1. Send Key Press Report */
        report[0] = modifier; /* Set Modifier (e.g., Shift) */
        report[2] = keycode;  /* Set Keycode */
        hid_injector_send_report(dev, report);

        msleep(20);

        /* 2. Send Key Release Report (all keys and modifiers up) */
        report[0] = 0;
        report[2] = 0;
        hid_injector_send_report(dev, report);
    }

    kfree(kbd_buf);
    return len;
}

static ssize_t dev_read(struct file *file, char __user *buffer, size_t len, loff_t *offset)
{
    const char *kernel_msg = "Hello World from the kernel space";
    size_t msg_len = strlen(kernel_msg);
    size_t to_copy;

    if (*offset >= msg_len) {
        return 0; /* End of file */
    }

    to_copy = min(len, msg_len - *offset);

    if (copy_to_user(buffer, kernel_msg + *offset, to_copy)) {
        return -EFAULT;
    }

    *offset += to_copy;
    return to_copy;
}

/*
 * Transparency notice: GenAI created function, this is to ensure debugging goes smoothly.
 * and to prevent my own mistakes from creating any misdebugged things.
 * Translates an ASCII character to a USB HID keycode and determines
 * if the Shift modifier is needed.
 *
 * @c: The character to translate.
 * @modifier: A pointer to a u8 that will be set to MOD_LEFT_SHIFT if needed.
 *
 * Returns: The HID keycode, or 0 for an unsupported character.
 */
static u8 char_to_hid_keycode(const char c, u8 *modifier)
{
    *modifier = 0; // Default to no modifier

    if (c >= 'a' && c <= 'z') {
        return 0x04 + (c - 'a');
    }
    if (c >= 'A' && c <= 'Z') {
        *modifier = MOD_LEFT_SHIFT;
        return 0x04 + (c - 'A');
    }
    if (c >= '1' && c <= '9') {
        return 0x1E + (c - '1');
    }

    switch (c) {
        case '0': return 0x27;
        case '\n': return 0x28; /* Enter */
        case '\t': return 0x2B; /* Tab */
        case ' ': return 0x2C;  /* Spacebar */
        case '-': return 0x2D;
        case '=': return 0x2E;
        case '[': return 0x2F;
        case ']': return 0x30;
        case '\\': return 0x31;
        case ';': return 0x33;
        case '\'': return 0x34;
        case '`': return 0x35;
        case ',': return 0x36;
        case '.': return 0x37;
        case '/': return 0x38;

        /* Characters requiring Shift */
        case '!': *modifier = MOD_LEFT_SHIFT; return 0x1E; /* 1 */
        case '@': *modifier = MOD_LEFT_SHIFT; return 0x1F; /* 2 */
        case '#': *modifier = MOD_LEFT_SHIFT; return 0x20; /* 3 */
        case '$': *modifier = MOD_LEFT_SHIFT; return 0x21; /* 4 */
        case '%': *modifier = MOD_LEFT_SHIFT; return 0x22; /* 5 */
        case '^': *modifier = MOD_LEFT_SHIFT; return 0x23; /* 6 */
        case '&': *modifier = MOD_LEFT_SHIFT; return 0x24; /* 7 */
        case '*': *modifier = MOD_LEFT_SHIFT; return 0x25; /* 8 */
        case '(': *modifier = MOD_LEFT_SHIFT; return 0x26; /* 9 */
        case ')': *modifier = MOD_LEFT_SHIFT; return 0x27; /* 0 */
        case '_': *modifier = MOD_LEFT_SHIFT; return 0x2D; /* - */
        case '+': *modifier = MOD_LEFT_SHIFT; return 0x2E; /* = */
        case '{': *modifier = MOD_LEFT_SHIFT; return 0x2F; /* [ */
        case '}': *modifier = MOD_LEFT_SHIFT; return 0x30; /* ] */
        case '|': *modifier = MOD_LEFT_SHIFT; return 0x31; /* \ */
        case ':': *modifier = MOD_LEFT_SHIFT; return 0x33; /* ; */
        case '"': *modifier = MOD_LEFT_SHIFT; return 0x34; /* ' */
        case '~': *modifier = MOD_LEFT_SHIFT; return 0x35; /* ` */
        case '<': *modifier = MOD_LEFT_SHIFT; return 0x36; /* , */
        case '>': *modifier = MOD_LEFT_SHIFT; return 0x37; /* . */
        case '?': *modifier = MOD_LEFT_SHIFT; return 0x38; /* / */

        default: return 0; /* Unsupported character */
    }
}

/*
 * completion callback for our sent USB requests.
 * This function is called by the UDC driver after a report is sent.
 * Its job is to free the memory we allocated for the request.
 */
static void hid_injector_complete(struct usb_ep *ep, struct usb_request *req)
{
    kfree(req->buf);
    usb_ep_free_request(ep, req);
}

/*
 * Builds and sends a single 8-byte HID report to the host.
 */
static int hid_injector_send_report(struct hid_injector_dev *dev, u8 *report)
{
    struct usb_request *req;
    int status;

    if (!dev->interface_active || !dev->in_ep) {
        return -ENODEV;
    }

    req = usb_ep_alloc_request(dev->in_ep, GFP_ATOMIC);
    if (!req) {
        return -ENOMEM;
    }

    req->buf = kmalloc(8, GFP_ATOMIC);
    if (!req->buf) {
        usb_ep_free_request(dev->in_ep, req);
        return -ENOMEM;
    }

    req->length = 8;
    req->complete = hid_injector_complete;
    req->context = dev;
    memcpy(req->buf, report, 8);

    status = usb_ep_queue(dev->in_ep, req, GFP_ATOMIC);
    if (status) {
        pr_err("%s: failed to queue hid report, status %d\n", DRIVER_NAME, status);
        /* If queueing fails, we must clean up immediately */
        kfree(req->buf);
        usb_ep_free_request(dev->in_ep, req);
    }

    return status;
}

/* --- Legacy Gadget Driver Implementation --- */

static int handle_string_request(struct usb_request *req, u8 index)
{
    const char *req_str;
    u16 len;
    u8 *buf = req->buf;
    int i;

    if (index == 0) {
        req->length = 4;
        buf[0] = 4;
        buf[1] = USB_DT_STRING;
        buf[2] = (u8)stringtab.language;
        buf[3] = (u8)(stringtab.language >> 8);
        return 4;
    }

    for (i = 0; strings[i].s; i++) {
        if (strings[i].id == index) {
            req_str = strings[i].s;
            goto found;
        }
    }
    return -EINVAL;

found:
    len = strlen(req_str);
    req->length = 2 + (len * 2);
    if (req->length > 255) {
        req->length = 255;
    }

    buf[0] = req->length;
    buf[1] = USB_DT_STRING;
    len = (req->length - 2) / 2;

    for (i = 0; i < len; i++) {
        buf[2 + (i * 2)] = req_str[i];
        buf[3 + (i * 2)] = 0;
    }
    return req->length;
}

static void hid_set_config_work_handler(struct work_struct *w)
{
    struct delayed_work *dwork = to_delayed_work(w);
    struct hid_injector_dev *dev = container_of(dwork, struct hid_injector_dev, set_config_work);
    const struct usb_endpoint_descriptor *ep_desc;
    struct usb_ep *ep;
    int status;

    pr_info("%s: --- Running set_config work handler ---\n", DRIVER_NAME);

    /* Locate our endpoint descriptor just for reference and to assign later */
    ep_desc = (const struct usb_endpoint_descriptor *)&config_desc_raw[27];

    /* --- NEW MATCHING LOGIC --- */
    /*
     * The dwc2 driver on this platform doesn't set ep->address, so we cannot
     * match by address. Instead, we find the first available endpoint that
     * matches the capabilities we need: Interrupt IN.
     */
    dev->in_ep = NULL;
    list_for_each_entry(ep, &dev->gadget->ep_list, ep_list) {
        /* Check for Interrupt and IN direction capabilities */
        if (ep->caps.type_int && ep->caps.dir_in) {
            pr_info("%s: Found compatible Interrupt IN endpoint: '%s'\n", DRIVER_NAME, ep->name);
            dev->in_ep = ep;
            break; /* Found it, stop searching */
        }
    }

    if (!dev->in_ep) {
        pr_err("%s: Failed to find any compatible Interrupt IN endpoint. Aborting.\n", DRIVER_NAME);
        return;
    }

    /*
     * CRITICAL: Now that we have the correct 'ep' object, we assign our
     * descriptor to it. This tells the UDC driver *how* to configure it
     * (max packet size, interval, etc.).
     */
    dev->in_ep->desc = ep_desc;
    dev->in_ep->driver_data = dev;

    status = usb_ep_enable(dev->in_ep);
    if (status == 0) {
        dev->interface_active = true;
        pr_info("%s: IN endpoint '%s' enabled successfully.\n", DRIVER_NAME, dev->in_ep->name);
    } else {
        pr_err("%s: Failed to enable IN endpoint '%s', status %d\n", DRIVER_NAME, dev->in_ep->name, status);
        dev->in_ep->desc = NULL;
        dev->in_ep = NULL;
    }
}

static int legacy_setup(struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
    struct hid_injector_dev *dev = dev_get_drvdata(&gadget->dev);
    struct usb_request *req = dev->req0;
    int value = -EOPNOTSUPP;
    u16 w_length = le16_to_cpu(ctrl->wLength);
    u16 w_value = le16_to_cpu(ctrl->wValue);
    u8 desc_type = w_value >> 8;
    u8 desc_idx = w_value & 0xff;

    req->zero = 0;
    req->complete = NULL;

    switch (ctrl->bRequestType) {
    case USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE:
        if (ctrl->bRequest == USB_REQ_GET_DESCRIPTOR) {
            switch (desc_type) {
            case USB_DT_DEVICE:
                value = min_t(unsigned, sizeof(device_desc), w_length);
                memcpy(req->buf, &device_desc, value);
                break;
            case USB_DT_CONFIG:
                value = min_t(unsigned, sizeof(config_desc_raw), w_length);
                memcpy(req->buf, &config_desc_raw, value);
                break;
            case USB_DT_STRING:
                value = handle_string_request(req, desc_idx);
                break;
            }
        }
        break;

    case USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE:
        if (ctrl->bRequest == USB_REQ_GET_DESCRIPTOR && desc_type == HID_DT_REPORT) {
            value = min_t(unsigned, sizeof(hid_report_desc), w_length);
            memcpy(req->buf, &hid_report_desc, value);
        }
        break;

    case USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE:
        if (ctrl->bRequest == USB_REQ_SET_CONFIGURATION && w_value == 1) {
            /* FIX: Schedule with a delay to avoid a race with the UDC driver. */
            schedule_delayed_work(&dev->set_config_work, msecs_to_jiffies(20));
            value = 0;
        }
        break;

    case USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE:
        /* The host can set idle rate, we just acknowledge it. */
        if (ctrl->bRequest == HID_REQ_SET_IDLE || ctrl->bRequest == HID_REQ_SET_PROTOCOL || ctrl->bRequest == HID_REQ_SET_REPORT) {
            value = 0;
        }
        break;
    }

    if (value >= 0) {
        req->length = value;
        req->zero = value < w_length;
        return usb_ep_queue(gadget->ep0, req, GFP_ATOMIC);
    }
    return value;
}

static void legacy_disconnect(struct usb_gadget *gadget)
{
    struct hid_injector_dev *dev = dev_get_drvdata(&gadget->dev);

    if (!dev) {
        return;
    }

    dev->interface_active = false;
    /* Use the sync version to ensure work is finished before we proceed. */
    cancel_delayed_work_sync(&dev->set_config_work);
    if (dev->in_ep) {
        usb_ep_disable(dev->in_ep);
    }
    pr_info("%s: gadget disconnected\n", DRIVER_NAME);
}

static void legacy_unbind(struct usb_gadget *gadget)
{
    struct hid_injector_dev *dev = dev_get_drvdata(&gadget->dev);

    /* It's possible unbind is called on a device that failed bind. Always check. */
    if (!dev) {
        return;
    }

    pr_info("%s: unbinding gadget and cleaning up resources\n", DRIVER_NAME);

    /*
     * Ensure any pending work is cancelled and has finished executing.
     * This is critical to prevent use-after-free bugs if the work handler
     * tries to run after we've freed memory.
     */
    cancel_delayed_work_sync(&dev->set_config_work);

    /* ---
     * The Official Cleanup Sequence (Reverse of legacy_bind)
     * ---
     */

    /*
     * Step 1: Destroy the device file.
     * This signals udev in user-space to remove /dev/hid_injector.
     * It must be done BEFORE destroying the class.
     */
    device_destroy(dev->dev_class, MKDEV(dev->major, 0));

    /*
     * Step 2: Destroy the device class.
     * This removes the /sys/class/hid_injector_class directory.
     * It must be done BEFORE unregistering the character device.
     */
    class_destroy(dev->dev_class);

    /*
     * Step 3: Unregister the character device driver.
     * This frees the major number that was allocated.
     */
    unregister_chrdev(dev->major, DEVICE_NAME);

    /* --- Final memory cleanup --- */
    kfree(dev->req0->buf);
    usb_ep_free_request(gadget->ep0, dev->req0);
    kfree(dev);

    /*
     * Finally, clear the driver data pointers to prevent stale references.
     */
    dev_set_drvdata(&gadget->dev, NULL);
    g_hid_dev = NULL;
}

static int legacy_bind(struct usb_gadget *gadget, struct usb_gadget_driver *driver)
{
    struct hid_injector_dev *dev;
    int status;

    /**
    This is where it gets complicated. You need to initialise the gadget API in very very specific ordering.
    This prevents race conditions and ensures we are aligned with the USB spec.

    If these are not done properly, then the USB device will fail to initialize.
     */

    // allocate memory for our state struct.
    // kZ alloc ensures that the allocation is zero filled, ensuring no issues with unintialised pointers.
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        return -ENOMEM;
    }

    // link our state struct with the kernel's gadget device.
    dev->gadget = gadget;
    dev_set_drvdata(&gadget->dev, dev);
    g_hid_dev = dev;

    // preallocate the EP0 request point.
    dev->req0 = usb_ep_alloc_request(gadget->ep0, GFP_ATOMIC);
    if (!dev->req0) {
        status = -ENOMEM;
        goto fail;
    }
    dev->req0->buf = kmalloc(256, GFP_ATOMIC);
    if (!dev->req0->buf) {
        status = -ENOMEM;
        goto fail_req0;
    }

    // this has to be async, due to a race condition with dwc2, which is setting up the USB gadget at this point.
    // if it is not async, then this will fail, causing an infinite reset loop, as DWC2 is not yet ready.
    INIT_DELAYED_WORK(&dev->set_config_work, hid_set_config_work_handler);


    /**
     * This portion enables the character device.
        A lot of error handling was implemented here, as an early debugging step.
     */
    // register the character device.
    dev->major = register_chrdev(0, DEVICE_NAME, &fops);
    if (dev->major < 0) {
        status = dev->major;
        pr_err("%s: failed to register char device, error %d\n", DRIVER_NAME, status);
        goto fail_req0_buf;
    }

    // register the character device class.
    dev->dev_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(dev->dev_class)) {
        status = PTR_ERR(dev->dev_class);
        pr_err("%s: failed to create device class, error %d\n", DRIVER_NAME, status);
        goto fail_chrdev;
    }

    // create the character device
    if (!device_create(dev->dev_class, NULL, MKDEV(dev->major, 0), NULL, DEVICE_NAME)) {
        status = -ENODEV;
        pr_err("%s: failed to create device file\n", DRIVER_NAME);
        goto fail_class;
    }

    pr_info("%s: gadget bound and ready\n", DRIVER_NAME);
    return 0;

fail_class:
    class_destroy(dev->dev_class);
fail_chrdev:
    unregister_chrdev(dev->major, DEVICE_NAME);
fail_req0_buf:
    kfree(dev->req0->buf);
fail_req0:
    usb_ep_free_request(gadget->ep0, dev->req0);
fail:
    kfree(dev);
    g_hid_dev = NULL;
    dev_set_drvdata(&gadget->dev, NULL);
    return status;
}

static struct usb_gadget_driver legacy_driver = {
    .function  = "HID Injector (Legacy)",
    .driver = { .name  = DRIVER_NAME, .owner = THIS_MODULE, },
    .bind      = legacy_bind,
    .unbind    = legacy_unbind,
    .setup     = legacy_setup,
    .disconnect= legacy_disconnect,
    .max_speed = USB_SPEED_FULL,
};

static int __init hid_injector_init(void)
{
    return usb_gadget_register_driver(&legacy_driver);
}

static void __exit hid_injector_exit(void)
{
    usb_gadget_unregister_driver(&legacy_driver);
}

module_init(hid_injector_init);
module_exit(hid_injector_exit);