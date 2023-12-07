#include <asm/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>

#include "occamy.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pulp Platform");
MODULE_DESCRIPTION("Occamy driver");

#undef pr_fmt
#define pr_fmt(fmt) "%s : " fmt, __func__

struct cardrv_private_data cardrv_data;

int read_snitch_cluster(struct cardev_private_data *dev_data,
                        struct device_node *np) {
    int err = 0;
    if (!np)
        return 1;
    err |= of_property_read_u32(np, "eth,compute-cores", &dev_data->n_cores);
    dev_data->n_clusters += 1;
    dev_data->n_quadrants += 1;
    return err;
}

int probe_node(struct platform_device *pdev,
               struct cardev_private_data *dev_data, struct shared_mem *result,
               const char *name, u32 map_id) {
    struct device_node *tmp_np;
    struct resource tmp_res;
    int err = 0;

    // Get the node in the DTS
    tmp_np = of_get_child_by_name(pdev->dev.of_node, name);
    if (tmp_np) {
        // Get addresses, remap them and save them to the struct shared_mem
        of_address_to_resource(tmp_np, 0, &tmp_res);
        result->vbase = devm_ioremap_resource(&pdev->dev, &tmp_res);
        if (IS_ERR(result->vbase)) {
            pr_err("Could not map %s io-region\n", name);
        } else {
            result->pbase = tmp_res.start;
            result->size = resource_size(&tmp_res);
            pr_info("Allocated %s io-region\n", name);
            // Try to access the memory region
            pr_info("Probing %s : %x\n", name, *((uint32_t *)result->vbase));
            if (*((uint32_t *)result->vbase) == 0xbadcab1e) {
                pr_warn("%s not found in hardware (0xbadcab1e)!\n", name);
                *result = (struct shared_mem) { 0 };
            }
            // Save the informations in the char device (for card_read)
            dev_data->buffer_size +=
                sprintf(dev_data->buffer + dev_data->buffer_size,
                        "%s: %px size = %x, (map_id = %u)\n", name,
                        result->pbase, result->size, map_id);
            // Check for additional parsing
            if (name == "snitch-cluster")
                err = read_snitch_cluster(dev_data, tmp_np);
        }
        return err;
    } else {
        pr_err("No %s in device tree\n", name);
    }
    *result = (struct shared_mem) { 0 };
    return 1;
}

// gets called when matched platform device
int card_platform_driver_probe(struct platform_device *pdev) {
    int ret, irq, i;
    struct cardev_private_data *dev_data;

    pr_info("A device is detected \n");

    // Memory managed and zero initialized alloc
    dev_data = devm_kzalloc(&pdev->dev, sizeof(*dev_data), GFP_KERNEL);
    if (!dev_data) {
        pr_info("Cannot allocate memory \n");
        return -ENOMEM;
    }

    // Save the device private data pointer in platform device structure
    dev_set_drvdata(&pdev->dev, dev_data);

    pr_info("Device size = %d\n", PDATA_SIZE);
    pr_info("Device permission = %d\n", PDATA_PERM);
    pr_info("Device serial number = %s\n", PDATA_SERIAL);
    pr_info("platform_device(%p) : name=%s ; id=%i ; id_auto=%i ; dev=%p(%s) ; "
            "num_ressources=%i ; id_entry=%p\n",
            pdev, pdev->name, pdev->id, pdev->id_auto, pdev->dev,
            pdev->dev.init_name, pdev->num_resources, pdev->id_entry);

    /* Dynamically allocate memory for the device buffer using size information
     * from the platform data */
    dev_data->buffer = devm_kzalloc(&pdev->dev, PDATA_SIZE, GFP_KERNEL);
    if (!dev_data->buffer) {
        pr_info("Cannot allocate memory \n");
        return -ENOMEM;
    }

    // Get the device number
    dev_data->dev_num = cardrv_data.device_num_base + pdev->id;
    // cdev init and cdev add
    cdev_init(&dev_data->cdev, &card_fops);
    dev_data->cdev.owner = THIS_MODULE;

    // Probe soc_ctrl
    probe_node(pdev, dev_data, &dev_data->soc_ctrl_mem, "soc-control",
               SOC_CTRL_MAPID);

    // Probe quadrant-control
    probe_node(pdev, dev_data, &dev_data->quadrant_ctrl_mem, "quadrant-control",
               QUADRANT_CTRL_MAPID);

    // Probe clint
    probe_node(pdev, dev_data, &dev_data->clint_mem, "clint", CLINT_MAPID);

    // Probe snitch-cluster
    probe_node(pdev, dev_data, &dev_data->snitch_cluster_mem, "snitch-cluster",
               SNITCH_CLUSTER_MAPID);

    // Probe scratchpad-narrow
    probe_node(pdev, dev_data, &dev_data->scratchpad_narrow_mem,
               "scratchpad_narrow", SCRATCHPAD_NARROW_MAPID);

    // Add hardware details to information buffer
    dev_data->buffer_size +=
        sprintf(dev_data->buffer + dev_data->buffer_size,
                "(n_cores, n_clusters, n_quadrants) = (%i, %i, %i)\n",
                dev_data->n_cores, dev_data->n_clusters, dev_data->n_quadrants);

    // Buffer list
    INIT_LIST_HEAD(&dev_data->test_head);

    ret = cdev_add(&dev_data->cdev, dev_data->dev_num, 1);
    if (ret < 0) {
        pr_err("Cdev add failed \n");
        return ret;
    }

    // Create device file for the detected platform device
    cardrv_data.device_card =
        device_create(cardrv_data.class_card, NULL, dev_data->dev_num, NULL,
                      "occamydev-%d", pdev->id);
    if (IS_ERR(cardrv_data.device_card)) {
        pr_err("Device create failed \n");
        ret = PTR_ERR(cardrv_data.device_card);
        cdev_del(&dev_data->cdev);
        return ret;
    }

    cardrv_data.total_devices++;

    pr_info("Probe was successful \n");

    return 0;
}

// gets called when the device is removed from the system
int card_platform_driver_remove(struct platform_device *pdev) {
    struct cardev_private_data *dev_data = dev_get_drvdata(&pdev->dev);

    // Remove a device that was created with device_create()
    device_destroy(cardrv_data.class_card, dev_data->dev_num);

    // Remove a cdev entry from the system
    cdev_del(&dev_data->cdev);

    cardrv_data.total_devices--;

    pr_info("A device is removed \n");

    return 0;
}

static const struct of_device_id carfield_of_match[] = {
    { .compatible = "eth,occamy-soc", }, {},
};
MODULE_DEVICE_TABLE(of, carfield_of_match);

struct platform_driver card_platform_driver = {
    .probe = card_platform_driver_probe,
    .remove = card_platform_driver_remove,
    .driver = { .name = "eth-occamy", .of_match_table = carfield_of_match, }
};

#define MAX_DEVICES 10

static int __init card_platform_driver_init(void) {
    int ret;

    // Dynamically allocate a device number for MAX_DEVICES
    ret = alloc_chrdev_region(&cardrv_data.device_num_base, 0, MAX_DEVICES,
                              "cardevs");
    if (ret < 0) {
        pr_err("Alloc chrdev failed \n");
        return ret;
    }

    // Create a device class under /sys/class
    cardrv_data.class_card = class_create(THIS_MODULE, "card_class");
    if (IS_ERR(cardrv_data.class_card)) {
        pr_err("Class creation failed \n");
        ret = PTR_ERR(cardrv_data.class_card);
        unregister_chrdev_region(cardrv_data.device_num_base, MAX_DEVICES);
        return ret;
    }

    platform_driver_register(&card_platform_driver);

    pr_info("card platform driver loaded \n");

    return 0;
}

static void __exit card_platform_driver_cleanup(void) {
    // Unregister the platform driver
    platform_driver_unregister(&card_platform_driver);

    // Class destroy
    class_destroy(cardrv_data.class_card);

    // Unregister device numbers for MAX_DEVICES
    unregister_chrdev_region(cardrv_data.device_num_base, MAX_DEVICES);

    pr_info("card platform driver unloaded \n");
}

module_init(card_platform_driver_init);
module_exit(card_platform_driver_cleanup);