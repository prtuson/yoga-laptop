/* Industrialio buffer test code.
 *
 * Copyright (c) 2008 Jonathan Cameron
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is primarily intended as an example application.
 * Reads the current buffer setup from sysfs and starts a short capture
 * from the specified device, pretty printing the result after appropriate
 * conversion.
 *
 * Command line parameters
 * generic_buffer -n <device_name> -t <trigger_name>
 * If trigger name is not specified the program assumes you want a dataready
 * trigger associated with the device and goes looking for it.
 *
 */

#define _GNU_SOURCE

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <linux/types.h>
#include <string.h>
#include <poll.h>
#include <endian.h>
#include <getopt.h>
#include <inttypes.h>
#include <syslog.h> /*pfps*/
#include "iio_utils.h"

/**
 * size_from_channelarray() - calculate the storage size of a scan
 * @channels:           the channel info array
 * @num_channels:       number of channels
 *
 * Has the side effect of filling the channels[i].location values used
 * in processing the buffer output.
 **/
int size_from_channelarray(struct iio_channel_info *channels, int num_channels)
{
        int bytes = 0;
        int i = 0;
        while (i < num_channels) {
                if (bytes % channels[i].bytes == 0)
                        channels[i].location = bytes;
                else
                        channels[i].location = bytes - bytes%channels[i].bytes
                                + channels[i].bytes;
                bytes = channels[i].location + channels[i].bytes;
                i++;
        }
        return bytes;
}

void print2byte(int input, struct iio_channel_info *info)
{
        /* First swap if incorrect endian */
        if (info->be)
                input = be16toh((uint16_t)input);
        else
                input = le16toh((uint16_t)input);

        /*
         * Shift before conversion to avoid sign extension
         * of left aligned data
         */
        input = input >> info->shift;
        if (info->is_signed) {
                int16_t val = input;
                val &= (1 << info->bits_used) - 1;
                val = (int16_t)(val << (16 - info->bits_used)) >>
                        (16 - info->bits_used);
                printf("SCALED %05f ", ((float)val + info->offset)*info->scale);
        } else {
                uint16_t val = input;
                val &= (1 << info->bits_used) - 1;
                printf("SCALED %05f ", ((float)val + info->offset)*info->scale);
        }
}
/**
 * process_scan() - print out the values in SI units
 * @data:               pointer to the start of the scan
 * @channels:           information about the channels. Note
 *  size_from_channelarray must have been called first to fill the
 *  location offsets.
 * @num_channels:       number of channels
 **/
void process_scan(char *data,
                  struct iio_channel_info *channels,
                  int num_channels)
{
        int k;
        for (k = 0; k < num_channels; k++) {
	  /*pfps printf("PROCESS SCAN channel %d bytes %d location %d signed %d data %x %d\n",k,
			  channels[k].bytes,channels[k].location,
			  channels[k].is_signed,(data+channels[k].location),*(data+channels[k].location));*/
                switch (channels[k].bytes) {
                        /* only a few cases implemented so far */
                case 2:
                        print2byte(*(uint16_t *)(data + channels[k].location),
                                   &channels[k]);
                        break;
                case 4:
                        if (!channels[k].is_signed) {
                                uint32_t val = *(uint32_t *)
                                        (data + channels[k].location);
                                printf("SCALED %05f ", ((float)val +
                                                 channels[k].offset)*
                                       channels[k].scale);
                        } else {
			  int32_t val = *(int32_t *) (data + channels[k].location);
			  /*pfps printf("VAL RAW %d %8x  ",channels[k].location,val); */
			  val = val >> channels[k].shift;
			  /*pfps printf("SHIFT %d %8x  ",channels[k].shift,val); */
			  if ( channels[k].bits_used < 32 ) val &= ((uint32_t)1 << channels[k].bits_used) - 1;
			  /*pfps printf("MASK %d %8x  ",channels[k].bits_used,val); */
			  val = (int32_t)(val << (32 - channels[k].bits_used)) >> (32 - channels[k].bits_used);
			  /*pfps printf("FIX %x\n",val); */
			  printf("%s %4d %6.1f  ", channels[k].name, 
				 val, ((float)val + channels[k].offset)* channels[k].scale);
			}
                        break;
                case 8:
                        if (channels[k].is_signed) {
                                int64_t val = *(int64_t *)
                                        (data +
                                         channels[k].location);
                                if ((val >> channels[k].bits_used) & 1)
                                        val = (val & channels[k].mask) |
                                                ~channels[k].mask;
                                /* special case for timestamp */
                                if (channels[k].scale == 1.0f &&
                                     channels[k].offset == 0.0f)
                                        printf("TIME %" PRId64 " ", val);
                                else
                                        printf("SCALED %05f ", ((float)val +
                                                         channels[k].offset)*
                                               channels[k].scale);
                        }
                        break;
                default:
                        break;
                }
	}
        printf("\n");
}

/**
 * enable_sensors: enable all the sensors in a device
 * @device_dir: the IIO device directory in sysfs
 * @
 **/
static int enable_sensors(const char *device_dir)
{
	DIR *dp;
	FILE *sysfsfp;
	int i;
	int ret;
	const struct dirent *ent;
	char *scan_el_dir;
	char *filename;

	ret = asprintf(&scan_el_dir, FORMAT_SCAN_ELEMENTS_DIR, device_dir);
	if (ret < 0) {
		ret = -ENOMEM;
		goto error_ret;
	}
	dp = opendir(scan_el_dir);
	if (dp == NULL) {
		ret = -errno;
		goto error_free_name;
	}
	while (ent = readdir(dp), ent != NULL) {
		if (strcmp(ent->d_name + strlen(ent->d_name) - strlen("_en"),
			   "_en") == 0) {
			ret = asprintf(&filename,
				       "%s/%s", scan_el_dir, ent->d_name);
			if (ret < 0) {
				ret = -ENOMEM;
				goto error_close_dir;
			}
			sysfsfp = fopen(filename, "r");
			if (sysfsfp == NULL) {
				ret = -errno;
				free(filename);
				goto error_close_dir;
			}
			fscanf(sysfsfp, "%d", &ret);
			fclose(sysfsfp);
			if ( !ret )
			  write_sysfs_int(ent->d_name,scan_el_dir,1);
			free(filename);
		}
	}
	return 0;
error_close_dir:
	closedir(dp);
error_free_name:
	free(scan_el_dir);
error_ret:
	return ret;
}



int main(int argc, char **argv)
{
        long num_loops = 2;
        unsigned long timedelay = 1000000;
        unsigned long buf_len = 128;

        int ret, c, i, j, toread;
        int fp;

        int num_channels;
        char *trigger_name = NULL, *device_name = NULL;
        char *dev_dir_name, *buf_dir_name;

        int datardytrigger = 1;
        char *data;
        ssize_t read_size;
        int dev_num, trig_num;
        char *buffer_access;
        int scan_size;
        int noevents = 0;
        char *dummy;

        struct iio_channel_info *channels;

        while ((c = getopt(argc, argv, "l:w:c:et:n:")) != -1) {
                switch (c) {
                case 'n':
                        device_name = optarg;
                        break;
                case 't':
                        trigger_name = optarg;
                        datardytrigger = 0;
                        break;
                case 'e':
                        noevents = 1;
                        break;
                case 'c':
                        num_loops = strtoul(optarg, &dummy, 10);
                        break;
                case 'w':
                        timedelay = strtoul(optarg, &dummy, 10);
                        break;
                case 'l':
                        buf_len = strtoul(optarg, &dummy, 10);
                        break;
                case '?':
                        return -1;
                }
        }

        if (device_name == NULL)
                return -1;

        /* Find the device requested */
        dev_num = find_type_by_name(device_name, "iio:device");
        if (dev_num < 0) {
                printf("Failed to find the %s\n", device_name);
                ret = -ENODEV;
                goto error_ret;
        }
        printf("iio device number being used is %d\n", dev_num);

        asprintf(&dev_dir_name, "%siio:device%d", iio_dir, dev_num);
	
	/* enable the sensors in the device */
	enable_sensors(dev_dir_name);

        if (trigger_name == NULL) {
                /*
                 * Build the trigger name. If it is device associated its
                 * name is <device_name>_dev[n] where n matches the device
                 * number found above
                 */
                ret = asprintf(&trigger_name,
                               "%s-dev%d", device_name, dev_num);
                if (ret < 0) {
                        ret = -ENOMEM;
                        goto error_ret;
                }
        }

        /* Verify the trigger exists */
        trig_num = find_type_by_name(trigger_name, "trigger");
        if (trig_num < 0) {
                printf("Failed to find the trigger %s\n", trigger_name);
                ret = -ENODEV;
                goto error_free_triggername;
        }
        printf("iio trigger number being used is %d\n", trig_num);

        /*
         * Parse the files in scan_elements to identify what channels are
         * present
         */
        ret = build_channel_array(dev_dir_name, &channels, &num_channels);
        if (ret) {
                printf("Problem reading scan element information\n");
                printf("diag %s\n", dev_dir_name);
                goto error_free_triggername;
        }

        /*
         * Construct the directory name for the associated buffer.
         * As we know that the lis3l02dq has only one buffer this may
         * be built rather than found.
         */
        ret = asprintf(&buf_dir_name,
                       "%siio:device%d/buffer", iio_dir, dev_num);
        if (ret < 0) {
                ret = -ENOMEM;
                goto error_free_triggername;
        }

	/*pfps*/ printf("DEV_DIR_NAME %s  TRIGGER_NAME %s\n",dev_dir_name,trigger_name);
        /*pfps printf("%s %s\n", dev_dir_name, trigger_name); */
        /* Set the device trigger to be the data ready trigger found above */
        ret = write_sysfs_string_and_verify("trigger/current_trigger",
                                        dev_dir_name,
                                        trigger_name);
        if (ret < 0) {
	  printf("Failed to write current_trigger file %d:%s\n",-ret,strerror(-ret));
                goto error_free_buf_dir_name;
        }

        /* Setup ring buffer parameters */
	/* pfps - don't do this, for now, as it may be causing problems
        ret = write_sysfs_int("length", buf_dir_name, buf_len);
        if (ret < 0)
                goto error_free_buf_dir_name;
	*/


        /* Enable the buffer */
        ret = write_sysfs_int("enable", buf_dir_name, 1);
        if (ret < 0)
                goto error_free_buf_dir_name;
        scan_size = size_from_channelarray(channels, num_channels);
	/*pfps*/ printf("SCAN_SIZE %d\n",scan_size);
        data = malloc(scan_size*buf_len);
        if (!data) {
                ret = -ENOMEM;
                goto error_free_buf_dir_name;
        }

        ret = asprintf(&buffer_access, "/dev/iio:device%d", dev_num);
        if (ret < 0) {
                ret = -ENOMEM;
                goto error_free_data;
        }

        /* Attempt to open non blocking the access dev */
        fp = open(buffer_access, O_RDONLY | O_NONBLOCK);
        if (fp == -1) { /* If it isn't there make the node */
                printf("Failed to open %s\n", buffer_access);
                ret = -errno;
                goto error_free_buffer_access;
        }

        /* Wait for events num_loops times */
        for (j = 0; j < num_loops; j++) {
                if (!noevents) {
                        struct pollfd pfd = {
                                .fd = fp,
                                .events = POLLIN,
                        };

                        poll(&pfd, 1, -1);
                        toread = buf_len;

                } else {
                        usleep(timedelay);
                        toread = 64;
                }

                read_size = read(fp,
                                 data,
                                 toread*scan_size);

		/*pfps printf("\nREAD SAMPLE:%02d SIZE:%d CHAN:%d ADDDRESS:%x DATA(H):%08x DATA(D):%d\n",
		  j,read_size,num_channels,data,*data,*data);
		          printf("                              ADDDRESS:%x DATA(H):%08x DATA(D):%d\n",
		  (data+4),*(data+4),*(data+4));
           printf("                              ADDDRESS:%x DATA(H):%08x DATA(D):%d\n",
	   (data+8),*(data+8),*(data+8)); */
                if (read_size == -EAGAIN) {
                        printf("nothing available\n");
                        continue;
                }
                for (i = 0; i < read_size/scan_size; i++) {
		  printf("N%03d: ",j);
		  process_scan(data + scan_size*i, channels, num_channels);
		}
        }

        /* Stop the buffer */
        ret = write_sysfs_int("enable", buf_dir_name, 0);
        if (ret < 0)
                goto error_close_buffer_access;

        /* Disconnect the trigger - just write a dummy name. */
        write_sysfs_string("trigger/current_trigger",
                        dev_dir_name, "NULL");

error_close_buffer_access:
        close(fp);
error_free_buffer_access:
        free(buffer_access);
error_free_data:
        free(data);
error_free_buf_dir_name:
        free(buf_dir_name);
error_free_triggername:
        if (datardytrigger)
                free(trigger_name);
error_ret:
        return ret;
}