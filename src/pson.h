// The MIT License (MIT)
//
// Copyright (c) 2015 THINGER LTD
// Author: alvarolb@gmail.com (Alvaro Luis Bustamante)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef PSON_HPP
#define PSON_HPP

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

namespace protoson {

    class memory_allocator{
    public:
        virtual void *allocate(size_t size) = 0;
        virtual void deallocate(void *) = 0;
    };

    template<size_t buffer_size>
    class circular_memory_allocator : public memory_allocator{
    private:
        uint8_t buffer_[buffer_size];
        size_t index_;
    public:
        circular_memory_allocator() : index_(0) {
        }

        virtual void *allocate(size_t size) {
            if (index_ + size > buffer_size) {
                index_ = 0;
            }
            void *position = &buffer_[index_];
            index_ += size;
            return position;
        }

        virtual void deallocate(void *) {

        }
    };

    class dynamic_memory_allocator : public memory_allocator{
    public:
        virtual void *allocate(size_t size) {
            return malloc(size);
        }

        virtual void deallocate(void *ptr) {
            free(ptr);
        }
    };

    extern memory_allocator& pool;
}

inline void* operator new(size_t sz, protoson::memory_allocator& a)
{
    return a.allocate(sz);
}

inline void operator delete(void* p, protoson::memory_allocator& a)
{
    a.deallocate(p);
}

template<class T>
void destroy(T* p, protoson::memory_allocator& a)
{
    if (p) {
        p->~T();
        a.deallocate(p);
    }
}

namespace protoson {

    enum pb_wire_type{
        varint = 0,
        fixed_64 = 1,
        length_delimited = 2,
        fixed_32 = 5
    };

    template<class T>
    class pson_container {

        struct list_item{
            T item_;
            list_item* next_;
        };

    public:

        class iterator{
        public:
            iterator(list_item *current) : current(current) {
            }

        private:
            list_item * current;

        public:
            bool next(){
                if(current==NULL) return false;
                current = current->next_;
                return true;
            }

            bool valid(){
                return current!=NULL;
            }

            T& item(){
                return current->item_;
            }
        };

    private:
        list_item* item_;

    public:
        iterator begin() const{
            return iterator(item_);
        }

        pson_container() : item_(NULL) {
        }

        ~pson_container(){
            list_item* current = item_;
            while(current!=NULL){
                list_item* next = current->next_;
                destroy(current, pool);
                current = next;
            }
        }

        T& create_item(){
            list_item* new_list_item = new(pool) list_item();
            if(item_==NULL){
                item_ = new_list_item;
            }
            else{
                list_item * last = item_;
                while(last->next_!=NULL) last = last->next_;
                last->next_ = new_list_item;
            }
            return new_list_item->item_;
        }
    };

    class pson_object;
    class pson_array;

    class pson {
    public:

        struct buffer_descriptor{
            buffer_descriptor() : buffer_(NULL), size_(0){}
            buffer_descriptor(void* buffer, size_t size) : buffer_(buffer), size_(size){}
            void* buffer_;
            size_t size_;
        };

        enum field_type {
            null_field      = 0,
            varint_field    = 1,
            svarint_field   = 2,
            float_field     = 3,
            double_field    = 4,
            true_field      = 5,
            false_field     = 6,
            zero_field      = 7,
            one_field       = 8,
            string_field    = 9,
            bytes_field     = 10,
            object_field    = 11,
            array_field     = 12
            // we have up to 2^4=16 (0-15) for encoding types
        };

        static pson empty_value;

        bool is_boolean(){
            return field_type_ == true_field || field_type_ == false_field;
        }

        bool is_string(){
            return field_type_ == string_field;
        }

        bool is_bytes(){
            return field_type_ == bytes_field;
        }

        bool is_number(){
            return  field_type_ == varint_field     ||
                    field_type_ == svarint_field    ||
                    field_type_ == float_field      ||
                    field_type_ == double_field     ||
                    field_type_ == zero_field       ||
                    field_type_ == one_field;
        }

        bool is_object(){
            return field_type_ == object_field;
        }

        bool is_array(){
            return field_type_ == array_field;
        }

        bool is_null(){
            return field_type_ == null_field;
        }

        pson() : field_type_(null_field), value_(NULL) {
        }

        template<class T>
        pson(T value) : field_type_(null_field), value_(NULL){
            *this = value;
        }

        ~pson(){
            if(field_type_==object_field){
                destroy((pson_object *) value_, pool);
            }else if(field_type_==array_field) {
                destroy((pson_array *) value_, pool);
            }else{
                pool.deallocate(value_);
            }
        }

        template<class T>
        void operator=(T value)
        {
            if(value==0){
                field_type_ = zero_field;
                return;
            }else if(value==1) {
                field_type_ = one_field;
                return;
            }

            if(value<0){
                field_type_ = svarint_field;
            }else{
                field_type_ = varint_field;
            }
            uint64_t uint_value = value>0 ? value : -value;
            value_ = pool.allocate(pson::get_varint_size(uint_value));
            pb_encode_varint((uint8_t*)value_, uint_value);
        }

        void operator=(bool value){
            field_type_ = value ? true_field : false_field;
        }

        void operator=(float value) {
            if(value==(int64_t)value){
                *this = (int64_t) value;
            }else{
                field_type_ = float_field;
                set(value);
            }
        }

        void operator=(double value) {
            if(value==(int64_t)value) {
                *this = (int64_t) value;
            }else if(fabs(value-(float)value)<=0.00001){
                *this = (float) value;
            }else{
                field_type_ = double_field;
                set(value);
            }
        }

        void operator=(const char *str) {
            field_type_ = string_field;
            memcpy(allocate(strlen(str)+1), str, strlen(str)+1);
        }

        operator const char *() const {
            if (field_type_ == string_field) {
                return (const char*) value_;
            }
            return "";
        }

        void operator=(buffer_descriptor desc){
            set_bytes(desc.buffer_, desc.size_);
        }

        void operator=(const buffer_descriptor& desc){
            set_bytes(desc.buffer_, desc.size_);
        }

        void set_bytes(const void* bytes, size_t size) {
            size_t varint_size = get_varint_size(size);
            value_ = pool.allocate(varint_size+size);
            pb_encode_varint((uint8_t*) value_, size);
            memcpy(((uint8_t*)value_)+varint_size, bytes, size);
            field_type_ = bytes_field;
        }

        operator buffer_descriptor(){
            buffer_descriptor desc;
            if(field_type_ == bytes_field){
                desc.size_ = pb_decode_varint((uint8_t*) value_);
                desc.buffer_ = (uint8_t*) value_ + get_varint_size(desc.size_);
            }
            return desc;
        }

        void* allocate(size_t size){
            value_ = pool.allocate(size);
            return value_;
        }

        operator pson_object &();
        operator pson_array &();
        pson & operator[](const char *name);
        const pson & operator[](const char *name) const;

        template<class T>
        operator T() const {
            switch(field_type_){
                case zero_field:
                case false_field:
                    return 0;
                case one_field:
                case true_field:
                    return 1;
                case float_field:
                    return *(float*)value_;
                case double_field:
                    return *(double*)value_;
                case varint_field:
                    return pb_decode_varint((uint8_t*)value_);
                case svarint_field:
                    return -pb_decode_varint((uint8_t*)value_);
                default:
                    return 0;
            }
        }

        void* get_value(){
            return value_;
        }

        void set_value(void* value){
            value_ = value;
        }

        field_type get_type(){
            return field_type_;
        }

        void set_type(field_type type){
            field_type_ = type;
        }

        size_t get_varint_size(uint64_t value) const{
            size_t size = 1;
            while(value>>=7) size++;
            return size;
        }

        void pb_encode_varint(uint8_t* buffer, uint64_t value) const
        {
            uint8_t count = 0;
            do
            {
                uint8_t byte = (uint8_t)(value & 0x7F);
                value >>= 7;
                if(value) byte |= 0x80;
                buffer[count] = byte;
                count++;
            }while(value);
        }

        uint64_t pb_decode_varint(uint8_t* buffer) const
        {
            if(buffer==NULL) return 0;
            uint64_t value = 0;
            uint8_t pos = 0;
            uint8_t byte = 0;
            do{
                byte = buffer[pos];
                value |= (uint64_t)(byte&0x7F) << pos*7;
                pos++;
            }while(byte>=0x80);
            return value;
        }

    private:
        void* value_;
        field_type field_type_;

        template<class T>
        void set(T value) {
            value_ = pool.allocate(sizeof(T));
            (*(T*)value_) = value;
        }
    };

    class pson_pair{
    private:
        char* name_;
        pson value_;
    public:
        pson_pair() : name_(NULL){
        }

        ~pson_pair(){
            destroy(name_, pool);
        }

        void set_name(const char *name) {
            size_t name_size = strlen(name) + 1;
            memcpy(allocate_name(name_size), name, name_size);
        }

        char* allocate_name(size_t size){
            name_ = (char*)pool.allocate(size);
            return name_;
        }

        pson &value() {
            return value_;
        }

        char* name(){
            return name_;
        }
    };

    class pson_object : public pson_container<pson_pair> {
    public:
        pson_object()
        {}

        pson &operator[](const char *name) {
            for(iterator it=begin(); it.valid(); it.next()){
                if(strcmp(it.item().name(), name)==0){
                    return it.item().value();
                }
            }
            pson_pair & pair = create_item();
            pair.set_name(name);
            return pair.value();
        };

        const pson & operator[](const char *name) const{
            for(iterator it=begin(); it.valid(); it.next()){
                if(strcmp(it.item().name(), name)==0){
                    return it.item().value();
                }
            }
            return pson::empty_value;
        };
    };

    class pson_array : public pson_container<pson> {
    public:
        pson_array()
        {}
    public:
        template<class T>
        void add(T item_value){
            create_item() = item_value;
        }
    };

    pson::operator pson_object &() {
        if (field_type_ != object_field) {
            value_ = new(pool) pson_object();
            field_type_ = object_field;
        }
        return *((pson_object *)value_);
    }

    pson::operator pson_array &() {
        if (field_type_ != array_field) {
            value_ = new(pool) pson_array();
            field_type_ = array_field;
        }
        return *((pson_array *)value_);
    }

    pson &pson::operator[](const char *name) {
        return ((pson_object &) *this)[name];
    }

    const pson &pson::operator[](const char *name) const{
        if(field_type_!=object_field){
            return empty_value;
        }
        return (*(pson_object *) value_)[name];
    }

    pson pson::empty_value;

    ////////////////////////////
    /////// PSON_DECODER ///////
    ////////////////////////////

    class pson_decoder {

    protected:
        size_t read_;

        virtual bool read(void* buffer, size_t size){
            read_+=size;
            return true;
        }

    public:

        pson_decoder() : read_(0) {

        }

        size_t bytes_read(){
            return read_;
        }

        bool pb_decode_tag(pb_wire_type& wire_type, uint32_t& field_number)
        {
            uint32_t temp = pb_decode_varint32();
            wire_type = (pb_wire_type)(temp & 0x07);
            field_number = temp >> 3;
            return true;
        }

        uint32_t pb_decode_varint32()
        {
            uint32_t varint = 0;
            uint8_t byte;
            uint8_t bit_pos = 0;
            do{
                if(!read(&byte, 1) || bit_pos>=32){
                    return varint;
                }
                varint |= (uint32_t)(byte&0x7F) << bit_pos;
                bit_pos += 7;
            }while(byte>0x80);
            return varint;
        }

        uint64_t pb_decode_varint64()
        {
            uint64_t varint = 0;
            uint8_t byte;
            uint8_t bit_pos = 0;
            do{
                if(!read(&byte, 1) || bit_pos>=64){
                    return varint;
                }
                varint |= (uint32_t)(byte&0x7F) << bit_pos;
                bit_pos += 7;
            }while(byte>0x80);
            return varint;
        }

        bool pb_skip(size_t size){
            uint8_t byte;
            bool success = true;
            for(size_t i=0; i<size; i++){
                success &= read(&byte, 1);
            }
            return success;
        }

        bool pb_skip_varint(){
            uint8_t byte;
            bool success = true;
            do{
                success &= read(&byte, 1);
            }while(byte>0x80);
            return success;
        }

        bool pb_read_string(char *str, size_t size){
            bool success = read(str, size);
            str[size]=0;
            return success;
        }

        bool pb_read_varint(pson& value)
        {
            char temp[10];
            uint8_t byte=0;
            uint8_t bytes_read=0;
            do{
                if(!read(&byte, 1)) return false;
                temp[bytes_read] = byte;
                bytes_read++;
            }while(byte>=0x80);
            memcpy(value.allocate(bytes_read), temp, bytes_read);
            return true;
        }

    public:

        void decode(pson_object & object, size_t size){
            size_t start_read = bytes_read();
            while(size-(bytes_read()-start_read)>0){
                decode(object.create_item());
            }
        }

        void decode(pson_array & array, size_t size){
            size_t start_read = bytes_read();
            while(size-(bytes_read()-start_read)>0){
                decode(array.create_item());
            }
        }

        void decode(pson_pair & pair){
            uint32_t name_size = pb_decode_varint32();
            pb_read_string(pair.allocate_name(name_size+1), name_size);
            decode(pair.value());
        }

        void decode(pson& value) {
            uint32_t field_number;
            pb_wire_type wire_type;
            pb_decode_tag(wire_type, field_number);
            value.set_type((pson::field_type)field_number);
            if(wire_type==pb_wire_type::length_delimited){
                uint32_t size = pb_decode_varint32();
                switch(field_number){
                    case pson::string_field:
                        pb_read_string((char*)value.allocate(size + 1), size);
                        break;
                    case pson::bytes_field:
                        value.allocate(size+value.get_varint_size(size));
                        value.pb_encode_varint((uint8_t*) value.get_value(), size);
                        read(((char*) value.get_value()) + value.get_varint_size(size), size);
                        break;
                    case pson::object_field:
                        value.set_value(new (pool) pson_object);
                        decode(*(pson_object *)value.get_value(), size);
                        break;
                    case pson::array_field:
                        value.set_value(new (pool) pson_array);
                        decode(*(pson_array *)value.get_value(), size);
                        break;
                    default:
                        pb_skip(size);
                        break;
                }
            }else {
                switch (field_number) {
                    case pson::svarint_field:
                    case pson::varint_field:
                        pb_read_varint(value);
                        break;
                    case pson::float_field:
                        read(value.allocate(4), 4);
                        break;
                    case pson::double_field:
                        read(value.allocate(8), 8);
                        break;
                    default:
                        break;
                }
            }
        }
    };

    ////////////////////////////
    /////// PSON_ENCODER ///////
    ////////////////////////////

    class pson_encoder {

    protected:
        size_t written_;

        virtual void write(const void* buffer, size_t size){
            written_+=size;
        }

    public:

        pson_encoder() : written_(0) {
        }

        size_t bytes_written(){
            return written_;
        }

        void pb_encode_tag(pb_wire_type wire_type, uint32_t field_number){
            uint64_t tag = ((uint64_t)field_number << 3) | wire_type;
            pb_encode_varint(tag);
        }

        void pb_encode_varint(uint32_t field, uint64_t value)
        {
            pb_encode_tag(varint, field);
            pb_encode_varint(value);
        }

        size_t pb_write_varint(void * buffer)
        {
            uint8_t byte=0;
            size_t bytes_written=0;
            do{
                byte = *((uint8_t*)buffer + bytes_written);
                write(&byte, 1);
                bytes_written++;
            }while(byte>=0x80);
            return bytes_written;
        }

        void pb_encode_varint(uint64_t value)
        {
            do
            {
                uint8_t byte = (uint8_t)(value & 0x7F);
                value >>= 7;
                if(value>0) byte |= 0x80;
                write(&byte, 1);
            }while(value>0);
        }

        void pb_encode_string(const char* str, uint32_t field_number){
            pb_encode_tag(length_delimited, field_number);
            pb_encode_string(str);
        }

        void pb_encode_string(const char* str){
            size_t string_size = strlen(str);
            pb_encode_varint(string_size);
            write(str, string_size);
        }

        template<class T>
        void pb_encode_submessage(T& element, uint32_t field_number)
        {
            pb_encode_tag(length_delimited, field_number);
            pson_encoder sink;
            sink.encode(element);
            pb_encode_varint(sink.bytes_written());
            encode(element);
        }

        void pb_encode_fixed32(void* value){
            write(value, 4);
        }

        void pb_encode_fixed64(void* value){
            write(value, 8);
        }

        void pb_encode_fixed32(uint32_t field, void*value)
        {
            pb_encode_tag(fixed_32, field);
            pb_encode_fixed32(value);
        }

        void pb_encode_fixed64(uint32_t field, void*value)
        {
            pb_encode_tag(fixed_64, field);
            pb_encode_fixed64(value);
        }

    public:

        void encode(pson_object & object){
            pson_container<pson_pair>::iterator it = object.begin();
            while(it.valid()){
                encode(it.item());
                it.next();
            }
        }

        void encode(pson_array & array){
            pson_container<pson>::iterator it = array.begin();
            while(it.valid()){
                encode(it.item());
                it.next();
            }
        }

        void encode(pson_pair & pair){
            pb_encode_string(pair.name());
            encode(pair.value());
        }

        void encode(pson & value) {
            switch (value.get_type()) {
                case pson::true_field:
                case pson::false_field:
                case pson::one_field:
                case pson::zero_field:
                    pb_encode_tag(varint, value.get_type());
                    break;
                case pson::string_field:
                    pb_encode_string((const char*)value.get_value(), pson::string_field);
                    break;
                case pson::bytes_field:
                    pb_encode_tag(length_delimited, pson::bytes_field);
                    write(((const char *)value.get_value()) + pb_write_varint(value.get_value()), value.pb_decode_varint((uint8_t *)value.get_value()));
                    break;
                case pson::svarint_field:
                case pson::varint_field:
                    pb_encode_tag(varint, value.get_type());
                    pb_write_varint(value.get_value());
                    break;
                case pson::float_field:
                    pb_encode_fixed32(pson::float_field, value.get_value());
                    break;
                case pson::double_field:
                    pb_encode_fixed64(pson::double_field, value.get_value());
                    break;
                case pson::object_field:
                    pb_encode_submessage(*(pson_object *) value.get_value(), pson::object_field);
                    break;
                case pson::array_field:
                    pb_encode_submessage(*(pson_array *) value.get_value(), pson::array_field);
                    break;
                default:
                    pb_encode_tag(varint, pson::null_field);
                    break;
            }
        }
    };
}

#endif