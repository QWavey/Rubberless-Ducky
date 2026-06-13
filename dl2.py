import urllib.request
from bs4 import BeautifulSoup

url = "https://payloadstudio.hak5.org/community/"
html = urllib.request.urlopen(url).read().decode('utf-8')
soup = BeautifulSoup(html, 'html.parser')
scripts = soup.find_all('script')

with open("c:/Users/flori/Downloads/firmware/scripts_dump2.txt", "w", encoding="utf-8") as f:
    for i, script in enumerate(scripts):
        if script.string and "Compile()" in script.string:
            f.write(f"--- SCRIPT {i} ---\n")
            f.write(script.string)
            f.write("\n\n")
