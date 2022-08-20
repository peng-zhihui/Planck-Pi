ls
apt install usbutils
nano etc/ssh/sshd_config
nano etc/ssh/sshd_config
nano /etc/init.d
nano /etc/init.d/runOnBoot
chmod +x /etc/init.d/runOnBoot
sudo ln -s /etc/init.d/runOnBoot /etc/rc2.d/S99runOnBoot
ln -s /etc/init.d/runOnBoot /etc/rc2.d/S99runOnBoot
nano /etc/fatab
nano /etc/fstab 
mkdir /opt/images/
rm -rf /opt/images/swap
dd if=/dev/zero of=/opt/images/swap bs=1024 count=512000
mkswap /opt/images/swap
swapon /opt/images/swap
free -m
apt clean
exit
passwd pi
exit
apt install sudo
nano sudoers
nano etc/sudoers
cd /
nano etc/sudoers
su pi
exit
nano /etc/hostname 
nano /etc/hostnames
nano /etc/hosts
nano /etc/resolv.conf
adduser pi
su pi
exit
reboot
su pi
exit
su
su pi
su pi
apt install wget
exit
swapoff /opt/images/swap
swapoff 
swapoff --help
swapoff -a
exit
