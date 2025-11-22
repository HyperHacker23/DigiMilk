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

const RAZORPAY_KEY_ID = process.env.RAZORPAY_KEY_ID || "YOUR_KEY_ID";
const RAZORPAY_KEY_SECRET = process.env.RAZORPAY_KEY_SECRET || "YOUR_KEY_SECRET";
const ESP32_BASE = process.env.ESP32_BASE || "http://192.168.4.1";
const DISPENSE_TOKEN = process.env.DISPENSE_TOKEN || "replace_with_shared_token";

const razorpay = new Razorpay({ key_id: RAZORPAY_KEY_ID, key_secret: RAZORPAY_KEY_SECRET });

// In-memory order store (demo). For production persist orders.
const orders = new Map();

app.post('/create_order', async (req, res) => {
    try {
        const { amount, currency = 'INR', client_value, client_mode } = req.body;
        if (!amount || !client_value || !client_mode) return res.status(400).json({ error: 'missing fields' });

        const options = {
            amount: Math.round(amount * 100), // paise
            currency,
            receipt: 'rcpt_' + Date.now(),
            payment_capture: 1,
            notes: { client_value: String(client_value), client_mode: client_mode }
        };
        const order = await razorpay.orders.create(options);
        orders.set(order.id, { amount: amount, client_value: client_value, client_mode: client_mode });
        res.json(order);
    } catch (err) {
        console.error('create_order err', err);
        res.status(500).json({ error: err.message });
    }
});

app.post('/verify_payment', async (req, res) => {
    try {
        const { razorpay_payment_id, razorpay_order_id, razorpay_signature } = req.body;
        if (!razorpay_payment_id || !razorpay_order_id || !razorpay_signature)
            return res.status(400).json({ error: 'missing fields' });

        // verify signature
        const generated_signature = crypto.createHmac('sha256', RAZORPAY_KEY_SECRET)
            .update(razorpay_order_id + '|' + razorpay_payment_id)
            .digest('hex');

        if (generated_signature !== razorpay_signature) {
            console.warn('Invalid signature', generated_signature, razorpay_signature);
            return res.status(400).json({ success: false, error: 'Invalid signature' });
        }

        // fetch payment and ensure captured
        const payment = await razorpay.payments.fetch(razorpay_payment_id);
        if (!payment) return res.status(500).json({ success: false, error: 'Could not fetch payment' });
        if (payment.status !== 'captured') {
            return res.status(400).json({ success: false, error: 'Payment not captured', status: payment.status });
        }

        const info = orders.get(razorpay_order_id);
        if (!info) {
            console.warn('Order mapping not found for', razorpay_order_id);
            return res.status(400).json({ success: false, error: 'Order not found' });
        }

        // trigger ESP32 (include token)
        const value = info.client_value;
        const mode = info.client_mode;
        const espUrl = `${ESP32_BASE}/start?value=${encodeURIComponent(value)}&mode=${encodeURIComponent(mode)}`;

        try {
            await axios.get(espUrl, { timeout: 5000, headers: { 'X-DISPENSE-TOKEN': DISPENSE_TOKEN } });
        } catch (e) {
            console.warn('Calling ESP32 failed', e.message);
            return res.json({ success: false, error: 'Payment verified but failed to reach dispenser: ' + e.message });
        }

        orders.delete(razorpay_order_id);
        return res.json({ success: true });
    } catch (err) {
        console.error('verify_payment err', err);
        res.status(500).json({ error: err.message });
    }
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log('Razorpay backend running on', PORT));
