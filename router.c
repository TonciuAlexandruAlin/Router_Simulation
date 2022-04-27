#include "queue.h"
#include "skel.h"

// longest prefix match
struct route_table_entry *get_best_route(uint32_t dest, struct route_table_entry* rtable, int left, int right) {
	while (left < right) {
		int mid = left + (right - left) / 2;
		if ((dest & rtable[mid].mask) == rtable[mid].prefix) {

			struct route_table_entry *maxMask = &rtable[mid];
			int old_mid = mid;
			// daca am gasit prefixul, caut masca cea mai mare in stanga
			while ((dest & rtable[mid - 1].mask) == rtable[mid - 1].prefix) {
				if (maxMask->mask < rtable[mid - 1].mask) {
					maxMask = &rtable[mid - 1];
					printf("da");
				}
				mid--;
			}
			mid = old_mid;
			// daca am gasit prexitul, caut masca cea mai mare in dreapta
			while ((dest & rtable[mid + 1].mask) == rtable[mid + 1].prefix) {
				if (maxMask->mask < rtable[mid + 1].mask) {
					maxMask = &rtable[mid + 1];
					printf("Dada");
				}
				mid++;
			}
			return maxMask;
		}
		else if (ntohl(dest & rtable[mid].mask) > ntohl(rtable[mid].prefix)) {
			left = mid + 1;
		}
		else {
			right = mid - 1;
		}
	}
	printf("nu");
	return NULL;
}

// compare pentru sortare crescator prefix, descrescator masca
int cmp(const void *a, const void *b) {
  if (ntohl((*(struct route_table_entry *)a).prefix) ==
      ntohl((*(struct route_table_entry *)b).prefix))
    return ntohl((*(struct route_table_entry *)b).mask) -
           ntohl((*(struct route_table_entry *)a).mask);
  return ntohl((*(struct route_table_entry *)a).prefix) -
           ntohl((*(struct route_table_entry *)b).prefix);
}

int main(int argc, char *argv[])
{
	packet m;
	int rc;

	setvbuf(stdout, NULL, _IONBF, 0);

	// Do not modify this line
	init(argc - 2, argv + 2);

	queue coada = queue_create();
	struct arp_entry *table_arp =  malloc(1000 * sizeof(struct arp_entry));
	int table_arp_len = 0;

	struct route_table_entry *rtable = malloc(100000 * sizeof(struct route_table_entry));
	int len = read_rtable(argv[1], rtable);

	// sortez tabela
    qsort(rtable, len, sizeof(struct route_table_entry), cmp);

	while (1) {
		rc = get_packet(&m);
		DIE(rc < 0, "get_packet");
		/* TODO */

		struct ether_header *eth_hdr = (struct ether_header *)m.payload;
		
        // daca este pachet de tip ip
        if (ntohs(eth_hdr->ether_type) == ETHERTYPE_IP){
            struct iphdr *ip_hdr = (struct iphdr *)(m.payload + sizeof (struct ether_header));

			// daca ttl e 0 sau 1 arunc pachet
			if (ip_hdr->ttl <= 1) {
				// icmp time exceeded
				continue;
			}
			if (ip_hdr->daddr == inet_addr(get_interface_ip(m.interface))) {
				// icmp ping reply
				continue;
			}
			// verific checksum nou cu checksum vechi
			uint16_t oldCheck;
            oldCheck = ip_hdr->check;
            ip_hdr->check = 0;
			if (oldCheck != (ip_checksum ((uint8_t *)ip_hdr, sizeof(struct iphdr)))) {
				continue;
			}

			// scad ttl si atribui checksum-ul nou
			ip_hdr->ttl--;
			ip_hdr->check = ip_checksum((uint8_t *)ip_hdr, sizeof(struct iphdr));
			struct route_table_entry *route = get_best_route(ip_hdr->daddr, rtable, 0, len - 1);
			if (route == NULL) {
				// icmp unreachable dest
				continue;
			}

			uint8_t mac[6];
			get_interface_mac(route->interface, mac);
			memcpy(eth_hdr->ether_shost, mac, 6);
			m.interface = route->interface;

			// daca nu gasesc in tabela arp, trimit un request
			int found = 0;
			for (int i = 0; i < table_arp_len && found == 0; i++) {
				if (table_arp[i].ip == route->next_hop) {
					memcpy(eth_hdr->ether_dhost, table_arp[i].mac, 6);
					found = 1;
					break;
				}
			}			

			if (found == 0) {
				// request daca nu gasesc ip-ul de care am nevoie
				// destinatia este ff:ff:ff:ff:ff:ff
				struct ether_header *request = (struct ether_header *) malloc(sizeof(struct ether_header));
				request->ether_type = htons(ETHERTYPE_ARP);
				uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
				memcpy(request->ether_dhost, broadcast, 6);
				packet pkt;
				struct arp_header arp_hdr2;
				arp_hdr2.htype = htons(1);
				arp_hdr2.ptype = htons(2048);
				arp_hdr2.op = htons(ARPOP_REQUEST);
				arp_hdr2.hlen = 6;
				arp_hdr2.plen = 4;
				memset(arp_hdr2.tha, 0, 6);
				get_interface_mac(m.interface, arp_hdr2.sha);
				memcpy(request->ether_shost, arp_hdr2.sha, 6);
				arp_hdr2.spa = inet_addr(get_interface_ip(m.interface));
				arp_hdr2.tpa = route->next_hop;
				memset(pkt.payload, 0, 1600);
				memcpy(pkt.payload, request, sizeof(struct ether_header));
				memcpy(pkt.payload + sizeof(struct ether_header), &arp_hdr2, sizeof(struct arp_header));
				pkt.len = sizeof(struct ether_header) + sizeof(struct arp_header);
				pkt.interface = m.interface;
				send_packet(&pkt);

				// adaug in coada pachetul pentru rutarea lui cand primesc informatia ceruta la request
				packet *addToQueue = (packet *) malloc(sizeof(packet));
				memcpy(addToQueue, &m, sizeof(packet));
				queue_enq(coada, addToQueue);
				continue;	
			}
			send_packet(&m);
		}
		// daca pachetul e de tip arp
        else if (ntohs(eth_hdr->ether_type) == ETHERTYPE_ARP) {
            struct arp_header *arp_hdr = (struct arp_header *)(m.payload + sizeof(struct ether_header));
		
			if(arp_hdr != NULL) {
				// daca e request
				if (ntohs(arp_hdr->op) == ARPOP_REQUEST) {
					struct ether_header *reply = (struct ether_header *) malloc(sizeof(struct ether_header));
					reply->ether_type = eth_hdr->ether_type;
					memcpy(reply->ether_dhost, eth_hdr->ether_shost, 6);
					packet packet;
					struct arp_header arp_hdr2;
					arp_hdr2.htype = htons(1);
					arp_hdr2.ptype = htons(2048);
					arp_hdr2.op = htons(2);
					arp_hdr2.hlen = 6;
					arp_hdr2.plen = 4;
					memcpy(arp_hdr2.tha, arp_hdr->sha, 6);
					get_interface_mac(m.interface, arp_hdr2.sha);
					memcpy(reply->ether_shost, arp_hdr2.sha, 6);
					arp_hdr2.spa = arp_hdr->tpa;
					arp_hdr2.tpa = arp_hdr->spa;
					memset(packet.payload, 0, 1600);
					memcpy(packet.payload, reply, sizeof(struct ether_header));
					memcpy(packet.payload + sizeof(struct ether_header), &arp_hdr2, sizeof(struct arp_header));
					packet.len = sizeof(struct ether_header) + sizeof(struct arp_header);
					packet.interface = m.interface;
					send_packet(&packet);

					continue;	
				}
				// daca e reply
				else if (ntohs(arp_hdr->op) == ARPOP_REPLY) {

					table_arp[table_arp_len].ip = arp_hdr->spa;
					for (int i = 0; i < 6; i++) {
						table_arp[table_arp_len].mac[i] = arp_hdr->sha[i]; 
					} 
					table_arp_len++;

					if(!queue_empty(coada)) {
						queue coada2 = queue_create();
						while(!queue_empty(coada)) {
							packet* p = queue_deq(coada);
							struct iphdr *ip_aux = (struct iphdr*)(p->payload + sizeof(struct ether_header));
							if (ip_aux != NULL) {
								struct route_table_entry *route = get_best_route(ip_aux->daddr, rtable, 0, len - 1);
								if (route->next_hop == arp_hdr->spa) {
									struct ether_header *ethhdr = (struct ether_header *)p->payload;
									get_interface_mac(p->interface, ethhdr->ether_shost);
									memcpy(ethhdr->ether_dhost, table_arp[table_arp_len - 1].mac, 6);
									send_packet(p);
								}
								else {
									queue_enq(coada2, p);
								}
							}
						}
						coada = coada2;
						continue;
					}
				}
			}
		}
	}
}