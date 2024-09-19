export primary_nodeip="172.11.0.2"
export standby1_nodeip="172.11.0.3"
export primary_nodename=primary
export standby1_nodename=standby1
export OG_NETWORK=og-network
export GS_PASSWORD=@Zl771023

echo "Primary: $primary_nodename   $primary_nodeip"
echo "Standby1: $standby1_nodename $standby1_nodeip"


docker run -d -it -P  --sysctl kernel.sem="250 6400000 1000 25600" --security-opt seccomp=unconfined \
-v /home/zhy/opengauss/opengauss-container/GaussData:/var/lib/opengauss/data --name opengauss-primary --net ${OG_NETWORK} --ip "$primary_nodeip" \
-h=$primary_nodename -e primaryhost="$primary_nodeip" -e primaryname="$primary_nodename" \
-e standbyhosts="$standby1_nodeip" -e standbynames="$standby1_nodename" -e GS_PASSWORD=$GS_PASSWORD \
opengauss:5.0.0

docker run -d -it -P  --sysctl kernel.sem="250 6400000 1000 25600" --security-opt seccomp=unconfined \
-v /home/zhy/opengauss/opengauss-container/GaussData:/var/lib/opengauss/data --name opengauss-standby --net ${OG_NETWORK} --ip "$standby1_nodeip" \
-h=$standby1_nodename -e primaryhost="$primary_nodeip" -e primaryname="$primary_nodename" \
-e standbyhosts="$standby1_nodeip" -e standbynames="$standby1_nodename" -e GS_PASSWORD=$GS_PASSWORD \
opengauss:5.0.0


echo "OpenGauss Database Docker Containers created."