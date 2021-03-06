/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   multi.h
 * Author: Verti
 *
 * Created on 27 janvier 2018, 14:26
 */
#include "multistructure.h"


#ifndef MULTI_H
#define MULTI_H

void time_string(char* string, int type);
void log_add(char* str, FILE* out);

char* get_value(char* param, char* string);
int get_param(char* param, char* string);

void socket_close(SOCKET sock);
int socket_close_all(SOCKET sock, Client* clients, int nbSlots);
void socket_send(SOCKET sock, const char* data);
int socket_read(SOCKET sock, char *buff);

int slot_client_ask(SOCKET sock);

SOCKET client_connection_init(const char *ipaddress);
int client_listen(SOCKET sock);

SOCKET server_connection_init(int nbClient);
void server_listen(SOCKET sock, int nb_slot, Slot* slots, Client* clients, SOCKET adminSock);
extern int server_start_rx(Slot* slot);
#endif /* MULTI_H */

