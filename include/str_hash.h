#ifndef __STR_HASH_H__
#define __STR_HASH_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_TABLE_SIZE 100

// Structure for each node in the hash table
typedef struct ds_str_node {
    char *key;
    struct ds_str_node *next;
} ds_str_node;

// Structure for the hash table
typedef struct {
    ds_str_node *table[DS_TABLE_SIZE];
    int num_entries;
} ds_str_hash;

// Hash function
static int hash(char *str)
{
    unsigned long hash = 5381;
    int c;
    while((c = *str++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return (hash % DS_TABLE_SIZE);
}

// Initialize a new hash set
static ds_str_hash *ds_str_hash_init()
{
    ds_str_hash *set = (ds_str_hash *)malloc(sizeof(*set));
    int i;

    for(i = 0; i < DS_TABLE_SIZE; i++) {
        set->table[i] = NULL;
    }
    set->num_entries = 0;

    return (set);
}

// Add a string to the set
static int ds_str_hash_add(ds_str_hash *set, char *str)
{
    int index = hash(str);
    ds_str_node *node = (ds_str_node *)malloc(sizeof(*node));
    node->key = strdup(str);
    node->next = NULL;

    ds_str_node **temp = &(set->table[index]);
    while(*temp != NULL) {
        if(strcmp((*temp)->key, node->key) == 0) {
            free(node->key);
            free(node);
            return (0); // duplicate - not added
        }
        temp = &((*temp)->next);
    }
    *temp = node;

    set->num_entries++;

    return (1); // added
}

// Check if a string is present in the set
static int ds_str_hash_find(ds_str_hash *set, char *str)
{
    int index = hash(str);
    ds_str_node *temp = set->table[index];
    while(temp != NULL) {
        if(strcmp(temp->key, str) == 0) {
            return 1; // Found
        }
        temp = temp->next;
    }
    return (0); // Not found
}

// Get all entries in the set
static int ds_str_hash_get_all(ds_str_hash *set, char ***entries)
{
    int n = 0;

    *entries = (char **)malloc(sizeof(*entries) * set->num_entries);

    for(int i = 0; i < DS_TABLE_SIZE; i++) {
        ds_str_node *temp = set->table[i];
        while(temp != NULL) {
            (*entries)[n++] = strdup(temp->key);
            temp = temp->next;
        }
    }

    return (set->num_entries);
}

// Free memory allocated for the hash set
static void ds_str_hash_free(ds_str_hash *set)
{
    for(int i = 0; i < DS_TABLE_SIZE; i++) {
        ds_str_node *temp = set->table[i];
        while(temp != NULL) {
            ds_str_node *prev = temp;
            temp = temp->next;
            free(prev->key);
            free(prev);
        }
    }
    free(set);
}

#endif //__STR_HASH_H__