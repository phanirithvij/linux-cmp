/*
 * Copyright (C) 2001 Lennert Buytenhek (buytenh@gnu.org) and 
 * James Leu (jleu@mindspring.net).
 * Copyright (C) 2001 by various other people who didn't put their name here.
 * Licensed under the GPL.
 */

#include "linux/kernel.h"
#include "linux/init.h"
#include "linux/netdevice.h"
#include "linux/etherdevice.h"
#include "net_kern.h"
#include "net_user.h"
#include "daemon.h"

struct daemon_init {
	char *sock_type;
	char *ctl_sock;
};

void daemon_init(struct net_device *dev, void *data)
{
	struct uml_net_private *pri;
	struct daemon_data *dpri;
	struct daemon_init *init = data;

	pri = dev->priv;
	dpri = (struct daemon_data *) pri->user;
	dpri->sock_type = init->sock_type;
	dpri->ctl_sock = init->ctl_sock;
	dpri->fd = -1;
	dpri->control = -1;
	dpri->dev = dev;
	/* We will free this pointer. If it contains crap we're burned. */
	dpri->ctl_addr = NULL;
	dpri->data_addr = NULL;
	dpri->local_addr = NULL;

	printk("daemon backend (uml_switch version %d) - %s:%s", 
	       SWITCH_VERSION, dpri->sock_type, dpri->ctl_sock);
	printk("\n");
}

static int daemon_read(int fd, struct sk_buff **skb, 
		       struct uml_net_private *lp)
{
	*skb = ether_adjust_skb(*skb, ETH_HEADER_OTHER);
	if(*skb == NULL) return(-ENOMEM);
	return(net_recvfrom(fd, (*skb)->mac.raw, 
			    (*skb)->dev->mtu + ETH_HEADER_OTHER));
}

static int daemon_write(int fd, struct sk_buff **skb,
			struct uml_net_private *lp)
{
	return(daemon_user_write(fd, (*skb)->data, (*skb)->len, 
				 (struct daemon_data *) &lp->user));
}

static struct net_kern_info daemon_kern_info = {
	.init			= daemon_init,
	.protocol		= eth_protocol,
	.read			= daemon_read,
	.write			= daemon_write,
};

int daemon_setup(char *str, char **mac_out, void *data)
{
	struct daemon_init *init = data;
	char *remain;

	*init = ((struct daemon_init)
		{ .sock_type 		= "unix",
		  .ctl_sock 		= "/tmp/uml.ctl" });
	
	remain = split_if_spec(str, mac_out, &init->sock_type, &init->ctl_sock,
			       NULL);
	if(remain != NULL)
		printk(KERN_WARNING "daemon_setup : Ignoring data socket "
		       "specification\n");
	
	return(1);
}

static struct transport daemon_transport = {
	.list 		= LIST_HEAD_INIT(daemon_transport.list),
	.name 		= "daemon",
	.setup  	= daemon_setup,
	.user 		= &daemon_user_info,
	.kern 		= &daemon_kern_info,
	.private_size 	= sizeof(struct daemon_data),
	.setup_size 	= sizeof(struct daemon_init),
};

static int register_daemon(void)
{
	register_transport(&daemon_transport);
	return(1);
}

__initcall(register_daemon);

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
