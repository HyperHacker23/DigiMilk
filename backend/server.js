// backend/server.js
const express = require('express');
const Razorpay = require('razorpay');
const bodyParser = require('body-parser');
const cors = require('cors');
const crypto = require('crypto');
const axios = require('axios');
require('dotenv').config();

const app = express();
app.use(cors());
app.use(bodyParser.json());

// Config
const RAZORPAY_KEY_ID = process.env.RAZORPAY_KEY_ID;
const RAZORPAY_KEY_SECRET = process.env.RAZORPAY_KEY_SECRET;
const ESP32_BASE = process.env.ESP32_BASE;
const DISPENSE_TOKEN = process.env.DISPENSE_TOKEN;

const razorpay = new Razorpay({ key_id: RAZORPAY_KEY_ID, key_secret: RAZORPAY_KEY_SECRET });

// Order Storage
const orders = new Map();

// 1. Create Order
app.post('/create_order', async (req, res) => {
    try {
        const { amount, client_value, client_mode } = req.body;

        const options = {
            amount: Math.round(amount * 100),
            currency: 'INR',
            receipt: 'rcpt_' + Date.now(),
            payment_capture: 1
        };

        const order = await razorpay.orders.create(options);

        orders.set(order.id, {
            amount: amount,
            client_value: client_value,
            client_mode: client_mode
        });

        // [BUG FIX] Memory Leak Prevention
        setTimeout(() => {
            if (orders.has(order.id)) orders.delete(order.id);
        }, 600000); // Delete after 10 mins

        res.json({
            id: order.id,
            currency: order.currency,
            amount: order.amount,
            key_id: RAZORPAY_KEY_ID
        });

    } catch (err) {
        console.error('Create Order Error:', err);
        res.status(500).json({ error: err.message });
    }
});

// 2. Verify Payment & Trigger ESP32
app.post('/verify_payment', async (req, res) => {
    try {
        const { razorpay_payment_id, razorpay_order_id, razorpay_signature } = req.body;

        const generated_signature = crypto.createHmac('sha256', RAZORPAY_KEY_SECRET)
            .update(razorpay_order_id + '|' + razorpay_payment_id)
            .digest('hex');

        if (generated_signature !== razorpay_signature) {
            return res.status(400).json({ success: false, error: 'Invalid signature' });
        }

        const info = orders.get(razorpay_order_id);
        if (!info) return res.status(400).json({ success: false, error: 'Order expired or not found' });

        console.log(`Verified! Dispensing: ${info.client_value} ${info.client_mode}`);

        // Trigger ESP32
        const params = new URLSearchParams();
        params.append('value', info.client_value);
        params.append('mode', info.client_mode);

        try {
            await axios.post(`${ESP32_BASE}/start`, params, {
                headers: {
                    'X-DISPENSE-TOKEN': DISPENSE_TOKEN,
                    'Content-Type': 'application/x-www-form-urlencoded'
                },
                timeout: 5000
            });

            orders.delete(razorpay_order_id);
            return res.json({ success: true, message: "Dispensing Started" });

        } catch (e) {
            console.error('ESP32 Offline:', e.message);
            return res.json({ success: false, error: 'Payment received, but dispenser did not respond.' });
        }

    } catch (err) {
        console.error('Verify Error:', err);
        res.status(500).json({ error: err.message });
    }
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`Server running on port ${PORT}`));