
out vec4 sk_FragColor;
void main() {
    float x = 0.0;
    switch (0) {
        case 0:
            x = 0.0;
            if (x < 1.0) break;
        case 1:
            x = 1.0;
    }
    sk_FragColor = vec4(x);
}
